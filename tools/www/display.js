// The page's side of the window server (wlserver/ in the source tree): the
// windows of the machine's Wayland clients, drawn from the machine's own
// memory.
//
// The server publishes a surface table in the machine's memory (wlserver/
// README.md: the layout) and says where it is on a BroadcastChannel. On every
// frame the browser draws, this reads the table, uploads what changed of
// every window's pixels (only the rectangles the client reported as damaged)
// straight from the shared memory into a WebGL texture, draws the windows in
// their order, and writes back, per window, which present it has uploaded
// and shown: the server fires the clients' frame callbacks and releases their
// buffers by that.
//
// Until a server announces a table, nothing here runs and the page is as it
// was: the kernel's frame buffer (the text console, X when it runs) is drawn
// by the runtime into the #screen canvas. With a server, #screen is given the
// window's size (the screen section below) and a WebGL canvas is laid over
// it (letting pointer events through to it, so input reaches the kernel as
// before), with the windows on it (the frame buffer is not drawn, unless
// ?console=1); a bar above has fullscreen, the keyboard layout and Cmd's
// meaning, a panel at the bottom the launcher, the windows and a clock. When
// the server's heartbeat stops, the overlay goes and #screen is all there is
// again, at its own size.
//
// Windows the server says are decorated get a frame drawn here, in the DOM
// over the canvas (chrome.css): a title bar with the buttons, a border to
// resize by. Dragging and the buttons become orders in the table (the
// orders ring), which the server carries out. A frame is clipped where a
// window above covers it, so that it neither shows nor takes clicks there.
// Everywhere else the pointer goes through to #screen and from there, by the
// runtime, into the machine (the server reads it from /dev/input).
//
// The pointer's picture over #screen is what the server says (a CSS cursor
// name, or an image as a url()). The clipboard: text a guest program copies
// is handed to navigator.clipboard.writeText; when the person presses Ctrl-V
// (with or without Shift) or Cmd-V on the screen, the host's clipboard text
// (from the paste event the key brings, or else navigator.clipboard.readText)
// goes to the server first and the key after it (wlserver/README.md: the
// clipboard, for what browsers allow when). The keys go through here while a
// server runs (the keyboard section: window keys, Cmd on a Mac), and files
// dropped on the screen go to the server (files dropped on the screen).
//
// Hooked into the page by one script tag:
//   <script src="display.js"></script> (after the runtime's, before the machine starts)
// It puts #screen into a stage of its own, with the canvas and the frames. chrome.css and panel.css, next to
// this file, are loaded from here.
// Exposed as window.rvDisplay: active(), shot() (a PNG data URL of the composed screen), stats, windows(),
// clipboard (what went where), order(), drop(), layout(), switcher(), windowList(), settings, fullscreen(),
// screenSize(), embed(), unembed(), activateEmbed(), switchFrom(), switchEnd() (pages shown in windows: embedFrame).
(() => {
  "use strict";

  // This page load's id, and the BroadcastChannels that carry it. A BroadcastChannel reaches every tab of the
  // origin, so a window server left running in an old tab (or a page still starting in another) would be heard by
  // this one: it showed the old server's splash and drew its table. So the channel between the page and the
  // machine is named for the page load, "rv-display-ID": window.rvChannel("rv-display") opens it. The machine
  // gets the id on the kernel's command line (rvtab=ID, tools/www/dev.html), and wlserver/boot.js reads it from
  // /proc/cmdline to name its own end of the channel the same way. (The other scripts of the page, loaded after
  // this one, use window.rvChannel too.)
  if (!window.rvTabId) {
    // (crypto.randomUUID is for secure contexts only; a page served on a LAN address gets 128 random bits in hex.)
    window.rvTabId = crypto.randomUUID ? crypto.randomUUID()
                     : Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => b.toString(16).padStart(2, "0")).join("");
    window.rvChannel = (name) => new BroadcastChannel(name + "-" + window.rvTabId);
  }

  const MAGIC = 0x4c445652;
  // No heartbeat for this long: the server is gone. Not seconds: the server is a process like any other,
  // and while a big program starts (Firefox) it can get no processor for ten seconds and more; its
  // windows must stay up meanwhile. A server that has really ended takes its table with it (the magic
  // and pid checks below see that at once).
  const DEAD_MS = 60000;
  // The layout (wlserver/README.md).
  const H_MAGIC = 0, H_PID = 8, H_HEARTBEAT = 12, H_SLOTS = 16, H_SLOT_SIZE = 20, H_SCREEN_W = 24, H_SCREEN_H = 28,
        H_GENERATION = 32, H_GUEST_SEL_SERIAL = 40, H_PAGE_HIDDEN = 44, H_PAGE_FRAME = 64, H_PAGE_TIME = 68, H_POINTER_X = 72, H_POINTER_Y = 76,
        H_CURSOR_SERIAL = 96, H_CURSOR_KIND = 100, H_CURSOR_SHAPE = 104, H_CURSOR_LO = 108, H_CURSOR_HI = 112,
        H_CURSOR_W = 116, H_CURSOR_H = 120, H_CURSOR_STRIDE = 124, H_CURSOR_HOT_X = 128, H_CURSOR_HOT_Y = 132,
        H_IME_STATE = 80, H_IME_X = 84, H_IME_Y = 88, H_IME_H = 92, H_TEXT_SERIAL = 1980, H_TEXT_ACK = 1984,
        H_TEXT_LEN = 1988, H_TEXT = 1992, TEXT_CAP = 56,
        H_TITLE_HEIGHT = 136, H_BORDER = 140, H_GUEST_CLIP_SERIAL = 144, H_GUEST_CLIP_LO = 148, H_GUEST_CLIP_HI = 152,
        H_GUEST_CLIP_LEN = 156, H_HOST_CLIP_LO = 160, H_HOST_CLIP_HI = 164, H_HOST_CLIP_CAP = 168,
        H_HOST_CLIP_SERIAL = 172, H_HOST_CLIP_LEN = 176, H_HOST_CLIP_ACK = 180, H_FOCUS = 184,
        H_OUT_SERIAL = 188, H_OUT_W = 192, H_OUT_H = 196, H_OUT_SCALE = 200, H_OUT_ACK = 204, H_SCREEN_SCALE = 208,
        H_KBD_SERIAL = 212, H_KBD_ACK = 216, H_KBD_STATUS = 220, H_KBD_NAME = 224,
        H_ORDER_HEAD = 256, H_ORDER_TAIL = 260, H_ORDERS = 264,
        H_KBD_CURRENT = 1288, H_XFER_LO = 1320, H_XFER_HI = 1324, H_XFER_CAP = 1328, H_XFER_SERIAL = 1332,
        H_XFER_LEN = 1336, H_XFER_FLAGS = 1340, H_XFER_TOTAL = 1344, H_XFER_X = 1348, H_XFER_Y = 1352,
        H_XFER_ACK = 1356, H_XFER_STATUS = 1360, H_XFER_NAME = 1368, H_XFER_PATH = 1624, H_DRAG = 1880,
        H_OUT_PANEL = 1884, H_APPS_LO = 1888, H_APPS_HI = 1892, H_APPS_LEN = 1896, H_APPS_SERIAL = 1900,
        H_LAUNCH_ACK = 1904, H_LAUNCH_STATUS = 1908,
        // the Plasma shell's words (wlserver/PLASMA-PROTOCOL.md)
        // the Plasma shell's words (wlserver/PLASMA-PROTOCOL.md; a server without the shell leaves shell_version 0)
        H_SHELL_VERSION = 1912, H_OUT_STRUT_TOP = 1916, H_OUT_STRUT_LEFT = 1920, H_OUT_STRUT_RIGHT = 1924,
        H_DESKTOPS = 1928, H_DESKTOP_CURRENT = 1932, H_DESKTOP_ROWS = 1936, H_DESKTOPS_LO = 1940, H_DESKTOPS_HI = 1944,
        H_DESKTOPS_LEN = 1948, H_DESKTOPS_SERIAL = 1952, H_SHOWING_DESKTOP = 1976;
  // The shell's orders (PLASMA-PROTOCOL.md): a = 2 toggles.
  const ORDER_KEEP_ABOVE = 11, ORDER_KEEP_BELOW = 12, ORDER_SHADE = 13, ORDER_ALL_DESKTOPS = 14, ORDER_TO_DESKTOP = 15,
        ORDER_SET_DESKTOP = 16, ORDER_GEOMETRY = 17, ORDER_TILE = 18, ORDER_SHOW_DESKTOP = 20, ORDER_DESKTOP_STEP = 21;
  const PANEL_HEIGHT = 32;	// the strip at the bottom of the screen the plain panel has (the server keeps windows off it)
  const ORDERS = 32, ORDER_SIZE = 32;
  const ORDER_ACTIVATE = 1, ORDER_MOVE = 2, ORDER_RESIZE = 3, ORDER_RESIZE_END = 4, ORDER_CLOSE = 5,
        ORDER_MINIMIZE = 6, ORDER_MAXIMIZE = 7, ORDER_RESTORE = 8, ORDER_FULLSCREEN = 9, ORDER_LAUNCH = 10;
  const F_ACTIVATED = 2, F_MAXIMIZED = 4, F_FULLSCREEN = 8, F_DECORATED = 32, F_MINIMIZED = 64;
  const KIND_TOPLEVEL = 1, KIND_POPUP = 2;
  // wp_cursor_shape_device_v1's shapes, by number, as CSS cursors
  const CURSORS = [null, "default", "context-menu", "help", "pointer", "progress", "wait", "cell", "crosshair", "text",
    "vertical-text", "alias", "copy", "move", "no-drop", "not-allowed", "grab", "grabbing", "e-resize", "n-resize",
    "ne-resize", "nw-resize", "s-resize", "se-resize", "sw-resize", "w-resize", "ew-resize", "ns-resize", "nesw-resize",
    "nwse-resize", "col-resize", "row-resize", "all-scroll", "zoom-in", "zoom-out", "copy", "move"];
  const S_SEQ = 0, S_ID = 4, S_PARENT = 8, S_KIND = 12, S_X = 16, S_Y = 20, S_W = 24, S_H = 28, S_Z = 32, S_FLAGS = 36,
        S_GEOM = 40, S_PRESENT = 56, S_BUFFER_LO = 60, S_BUFFER_HI = 64, S_STRIDE = 68, S_FORMAT = 72, S_BUF_W = 76,
        S_BUF_H = 80, S_SCALE = 84, S_DAMAGE_COUNT = 88, S_DAMAGE = 92, S_UPLOADED = 224, S_SHOWN = 228,
        S_TITLE = 256, S_APP_ID = 320, S_DESKTOP = 232, S_SHELL_FLAGS = 236, S_TILE = 240, S_GPU_SERIAL = 244,
        S_LAYER = 248, S_ROLE = 252;
  const KIND_SUBSURFACE = 3;
  // A layer surface (wlserver/layershell.js): plasmashell's desktop and panels, drawn as they are, no frame.
  const KIND_LAYER = 6;
  // The header's flags word: 1 the desktop's shell is plasmashell's (wlserver/plasmashell.js), not the page's.
  const H_FLAGS = 36, FLAG_NATIVE_SHELL = 1;
  const FORMAT_XRGB8888 = 1;
  // A window drawn by the GPU (gpu/PROTOCOL.md, stage B): the slot's buffer_lo is a resource (the GPU server's
  // numbers, global) and buffer_hi the GPU server's process id, the pixels are a texture of the GPU executor (gpu-exec.js, window.rvGpu), nothing is uploaded;
  // the frame is drawn once the GPU ring's `done` has passed the slot's gpu_serial. Formats 3 and 4 are
  // buffers in memory with bytes R, G, B, A (ABGR8888, XBGR8888: 4 is opaque): uploaded, not swizzled.
  const FORMAT_GPU = 2, FORMAT_ABGR8888 = 3, FORMAT_XBGR8888 = 4;

  // ---- the kernel's frame buffer: where it is, overheard from the runtime (patch-runtime.py item 4: the
  // Worker tells the main thread with an fb_show message, which linux.js keeps to itself).
  let fb = null;
  {
    const proto = Worker.prototype;
    const handler = Object.getOwnPropertyDescriptor(proto, "onmessage");
    if (handler && handler.set) {
      Object.defineProperty(proto, "onmessage", {
        configurable: true,
        get() {
          return handler.get.call(this);
        },
        set(f) {
          handler.set.call(this, typeof f != "function" ? f : function (event) {
            const d = event.data;
            if (d && d.method === "fb_show") {
              fb = { pixels: d.pixels, width: d.width, height: d.height, stride: d.stride };
            }
            return f.call(this, event);
          });
        },
      });
    }
  }

  // ---- the page's choices: ?screen=WxH (a fixed size, else the screen fills the window below the bar),
  // ?scale=N (else devicePixelRatio, rounded), ?console=1 (the kernel's frame buffer stays visible behind the
  // windows, letterboxed; else it is not drawn while a window server runs), ?kbd=LAYOUT, ?cmd=ctrl|super.
  const params = new URLSearchParams(location.search);
  const fixedSize = /^(\d+)x(\d+)$/.exec(params.get("screen") || "");
  const scaleParam = Number(params.get("scale")) || 0;
  const showConsole = params.get("console") === "1";
  // ?chrome=plain: the plain frames and panel; else Plasma's look, configured by Plasma's files in the guest
  // (plasma.js, breeze.css, loaded by setupChrome; wlserver/plasma.js reads the files for it).
  const plainChrome = params.get("chrome") === "plain";
  const theme = () => (!plainChrome && window.rvPlasma && window.rvPlasma.ready() ? window.rvPlasma : null);
  // ?shell=plasma, or the server's word for it: plasmashell is the desktop's shell (`desktop wayland plasma-native`).
  // Then the page draws no panel and no wallpaper of its own and keeps no strips for them; plasmashell's desktop and
  // panels are layer surfaces in the table, drawn with the windows.
  const shellParam = params.get("shell") === "plasma";
  let nativeShell = shellParam;
  const panelHeight = () => {
    const T = theme();
    return nativeShell ? 0 : T ? T.panel.strip() : PANEL_HEIGHT;
  };
  const pageStruts = () => {
    const T = theme();
    return nativeShell ? { top: 0, left: 0, right: 0, bottom: 0 } : T ? T.panel.struts() : null;
  };
  const isMac = /Mac|iPhone|iPad/.test(navigator.platform || "")
    || !!(navigator.userAgentData && /mac/i.test(navigator.userAgentData.platform || ""));
  const settings = (() => {
    let saved = {};
    try {
      saved = JSON.parse(localStorage.getItem("rv-display") || "{}") || {};
    } catch (error) {
      saved = {};
    }
    return { layout: saved.layout || null, cmdAsCtrl: saved.cmdAsCtrl === undefined ? isMac : !!saved.cmdAsCtrl };
  })();
  if (params.get("cmd")) {
    settings.cmdAsCtrl = params.get("cmd") === "ctrl";
  }
  const saveSettings = () => {
    try {
      localStorage.setItem("rv-display", JSON.stringify(settings));
    } catch (error) {
      // (no storage here: the choice lasts as long as the page)
    }
  };

  // ---- state
  let os = null;		// window.linuxOs, once the machine runs

  // The doorbell: after the page has written something into the table that the server is to act on (a present
  // uploaded or shown, an order, the clipboard, a screen, a layout, a piece of a file), it rings, and the server's
  // wait ends at once instead of at its next turn (an input event of kind 5: the kernel's "Wasm host" device,
  // /dev/input/event2, which the server watches; wlserver/README.md). Rung once for all that one frame wrote.
  let bellQueued = false;
  const ringBell = () => {
    if (!bellQueued && os && os.screen_input) {
      bellQueued = true;
      queueMicrotask(() => {
        bellQueued = false;
        os.screen_input(5, 1, 0);
      });
    }
  };
  let table = null;		// { address, pid, slots, slotSize, header, width, height }
  let active = false;
  let lastBeat = -1, lastBeatAt = 0;
  let canvas = null, gl = null, program = null, uniforms = null, vao = null;
  let sharedViews = true;	// WebGL takes views over shared memory (if not: copies)
  const surfaces = new Map();	// slot -> { id, texture, w, h, present }
  const consoleSurface = { texture: null, w: 0, h: 0 };
  const stats = {
    frames: 0, uploads: 0, uploadBytes: 0, fullBytes: 0, consoleBytes: 0, lastFrameBytes: 0, lastFrameFull: 0,
    lastFrameRects: 0, windows: 0, fps: 0, perFrame: 0, perFrameFull: 0, perFrameConsole: 0, startedAt: 0, lastReport: 0,
    reportFrames: 0, reportBytes: 0, reportFull: 0, reportConsole: 0, sharedViews: true, log: [],
  };
  const decoder = new TextDecoder("utf-8", { fatal: false });

  const channel = window.rvChannel("rv-display");
  channel.onmessage = (event) => {
    const m = event.data;
    if (!m || m.type !== "table") {
      return;
    }
    // (Another server's announcement while ours lives: not taken; the page stays with the one it draws.)
    if (table && active && performance.now() - lastBeatAt < DEAD_MS && (table.address !== m.address || table.pid !== m.pid)) {
      return;
    }
    if (!table || table.address !== m.address || table.pid !== m.pid) {
      table = m;
      lastBeat = -1;
      lastBeatAt = performance.now();
      say("display: a window server at " + m.address + " (pid " + m.pid + "), " + m.width + "x" + m.height);
    }
    if (document.hidden) {
      tellHidden();	// (a server started, or announcing itself, while the page is not shown: it is told now)
    }
  };

  // ---- while the page is not shown
  //
  // A page the browser does not show (another tab or another app in front, the window minimized or on another
  // Space) gets no frames: requestAnimationFrame waits until it is shown again, so this draws nothing and
  // acknowledges no presents, and the window server's clients, waiting for their frame callbacks and their
  // buffers, stood still (a GPU program as much as any). So the page tells the server, in the table's
  // page_hidden word, that it is hidden and how many frames a second to stand in for it with (?hiddenfps=N, 20
  // if not said; 0: none, the clients wait as before), and the server's own turns, which run in the machine
  // and not on this thread, fire the frame callbacks at that rate (wlserver/compositor.js). Nothing is drawn
  // meanwhile; when the page is shown again every window is uploaded whole (what the clients drew meanwhile is
  // not in the damage of the last present alone), and the GPU's Worker sends frames again.
  const HIDDEN_FPS = (() => {
    const asked = new URLSearchParams(location.search).get("hiddenfps");
    return asked !== null ? Math.max(0, Math.min(1000, Number(asked) | 0)) : 20;
  })();
  const tellHidden = () => {
    if (!os && window.linuxOs) {
      os = window.linuxOs;
    }
    if (!table || !os || !os.memory) {
      return;
    }
    const buffer = os.memory.buffer;
    const t = table.address;
    if (t + table.header > buffer.byteLength || new DataView(buffer).getUint32(t + H_MAGIC, true) !== MAGIC) {
      return;
    }
    Atomics.store(new Int32Array(buffer), (t + H_PAGE_HIDDEN) / 4, document.hidden ? HIDDEN_FPS : 0);
  };
  document.addEventListener("visibilitychange", () => {
    tellHidden();
    if (window.rvGpu && window.rvGpu.hidden) {
      window.rvGpu.hidden(document.hidden);
    }
    if (document.hidden) {
      stats.hiddenCount = (stats.hiddenCount || 0) + 1;
      stats.hiddenAt = performance.now();
    } else {
      for (const surface of surfaces.values()) {
        surface.present = 0;	// (all of every window again at the next frame)
      }
      if (stats.hiddenAt) {
        stats.hiddenMs = (stats.hiddenMs || 0) + performance.now() - stats.hiddenAt;
        stats.hiddenAt = 0;
      }
    }
  });

  const say = (text) => {
    console.log(text);
    stats.log.push(text);
    if (stats.log.length > 50) {
      stats.log.shift();
    }
  };

  // ---- WebGL
  const VERTEX = `#version 300 es
    in vec2 corner;
    uniform vec4 rect;      // x, y, w, h in screen pixels
    uniform vec2 screen;
    uniform int flip;       // 1: the texture's first row is the image's bottom (a GL render target: GPU windows)
    out vec2 uv;
    void main() {
      uv = flip == 1 ? vec2(corner.x, 1.0 - corner.y) : corner;
      vec2 p = (rect.xy + corner * rect.zw) / screen * 2.0 - 1.0;
      gl_Position = vec4(p.x, -p.y, 0.0, 1.0);
    }`;
  const FRAGMENT = `#version 300 es
    precision mediump float;
    in vec2 uv;
    uniform sampler2D image;
    uniform int swizzle;    // 1: the bytes are B, G, R, A (Wayland's ARGB8888/XRGB8888 words)
    uniform int opaque;     // 1: alpha is not meant (XRGB, the frame buffer)
    uniform float fade;     // the effects' opacity (1 but while a window opens, closes, ...)
    out vec4 color;
    void main() {
      vec4 c = texture(image, uv);
      if (swizzle == 1) { c = c.bgra; }
      if (opaque == 1) { c.a = 1.0; }
      color = c * fade;      // (ARGB8888 is premultiplied: blended as ONE, ONE_MINUS_SRC_ALPHA)
    }`;

  // The stage: #screen (which takes the pointer and the keys, for the runtime and for us), the canvas over it,
  // and the frames and the switcher over that, in one box that can go fullscreen as a whole. The bar above it
  // has the page's own controls (shown while a window server runs).
  let stage = null, bar = null;
  const setup = () => {
    const sc = screen();
    stage = document.createElement("div");
    stage.id = "rv-stage";
    bar = document.createElement("div");
    bar.id = "rv-bar";
    bar.style.display = "none";
    if (sc && sc.parentNode) {
      sc.parentNode.insertBefore(bar, sc);
      sc.parentNode.insertBefore(stage, sc);
      stage.appendChild(sc);
    } else {
      document.body.appendChild(stage);
    }
    canvas = document.createElement("canvas");
    canvas.id = "display-canvas";
    canvas.style.cssText = "position: absolute; left: 0; top: 0; display: none; pointer-events: none; background: #1d2b36;";
    stage.appendChild(canvas);
    gl = canvas.getContext("webgl2", { alpha: false, antialias: false, premultipliedAlpha: true, preserveDrawingBuffer: false });
    if (!gl) {
      say("display: no WebGL2 here; windows cannot be drawn");
      return false;
    }
    const shader = (type, source) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, source);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
        throw new Error("display: shader: " + gl.getShaderInfoLog(s));
      }
      return s;
    };
    program = gl.createProgram();
    gl.attachShader(program, shader(gl.VERTEX_SHADER, VERTEX));
    gl.attachShader(program, shader(gl.FRAGMENT_SHADER, FRAGMENT));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error("display: program: " + gl.getProgramInfoLog(program));
    }
    gl.useProgram(program);
    uniforms = {
      rect: gl.getUniformLocation(program, "rect"), screen: gl.getUniformLocation(program, "screen"),
      image: gl.getUniformLocation(program, "image"), swizzle: gl.getUniformLocation(program, "swizzle"),
      opaque: gl.getUniformLocation(program, "opaque"), fade: gl.getUniformLocation(program, "fade"),
      flip: gl.getUniformLocation(program, "flip"),
    };
    gl.uniform1f(uniforms.fade, 1);
    // (Our own vertex array: a GPU executor sharing this context binds its own.)
    vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const corners = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, corners);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]), gl.STATIC_DRAW);
    const corner = gl.getAttribLocation(program, "corner");
    gl.enableVertexAttribArray(corner);
    gl.vertexAttribPointer(corner, 2, gl.FLOAT, false, 0, 0);
    gl.uniform1i(uniforms.image, 0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    return true;
  };

  const newTexture = (w, h) => {
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    return t;
  };

  // One rectangle of a buffer into the bound texture. The source is a view over the machine's memory;
  // the row length and the skips pick the rectangle out of it.
  let scratch = null;
  const upload = (buffer, address, stride, x, y, w, h) => {
    const bytes = stride * (y + h);
    if (address + bytes > buffer.byteLength) {
      return 0;
    }
    if (sharedViews) {
      try {
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, stride >> 2);
        gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, x);
        gl.pixelStorei(gl.UNPACK_SKIP_ROWS, y);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, w, h, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(buffer, address, bytes), 0);
        return w * h * 4;
      } catch (error) {
        sharedViews = false;
        stats.sharedViews = false;
        say("display: WebGL does not take views over the shared memory (" + error.message + "): pixels are copied");
      }
    }
    // Copied: the rectangle's rows, one after the other.
    const need = w * h * 4;
    if (!scratch || scratch.length < need) {
      scratch = new Uint8Array(Math.max(need, 1 << 20));
    }
    const src = new Uint8Array(buffer, address, bytes);
    for (let row = 0; row < h; row++) {
      scratch.set(src.subarray((y + row) * stride + x * 4, (y + row) * stride + (x + w) * 4), row * w * 4);
    }
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
    gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
    gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, w, h, gl.RGBA, gl.UNSIGNED_BYTE, scratch, 0);
    return need;
  };

  // A slot, read whole (again if the server was writing it).
  const readSlot = (v, w32, at) => {
    for (;;) {
      const seq1 = Atomics.load(w32, at / 4);
      if (seq1 & 1) {
        continue;
      }
      const s = {
        id: v.getUint32(at + S_ID, true), parent: v.getUint32(at + S_PARENT, true), kind: v.getUint32(at + S_KIND, true),
        x: v.getInt32(at + S_X, true), y: v.getInt32(at + S_Y, true), w: v.getUint32(at + S_W, true), h: v.getUint32(at + S_H, true),
        z: v.getUint32(at + S_Z, true), flags: v.getUint32(at + S_FLAGS, true),
        geometry: [v.getInt32(at + S_GEOM, true), v.getInt32(at + S_GEOM + 4, true), v.getInt32(at + S_GEOM + 8, true),
                   v.getInt32(at + S_GEOM + 12, true)],
        present: v.getUint32(at + S_PRESENT, true),
        buffer: v.getUint32(at + S_BUFFER_LO, true) + v.getUint32(at + S_BUFFER_HI, true) * 0x100000000,
        stride: v.getUint32(at + S_STRIDE, true), format: v.getUint32(at + S_FORMAT, true),
        bw: v.getUint32(at + S_BUF_W, true), bh: v.getUint32(at + S_BUF_H, true),
        scale: v.getUint32(at + S_SCALE, true) || 1,
        damageCount: v.getUint32(at + S_DAMAGE_COUNT, true), damage: null, at,
        desktopRaw: v.getUint32(at + S_DESKTOP, true), shellRaw: v.getUint32(at + S_SHELL_FLAGS, true),
        layer: v.getUint32(at + S_LAYER, true), role: v.getUint32(at + S_ROLE, true),
      };
      if (s.damageCount !== 0xffffffff) {
        s.damage = [];
        for (let d = 0; d < Math.min(s.damageCount, 8); d++) {
          s.damage.push([v.getInt32(at + S_DAMAGE + 16 * d, true), v.getInt32(at + S_DAMAGE + 16 * d + 4, true),
                         v.getInt32(at + S_DAMAGE + 16 * d + 8, true), v.getInt32(at + S_DAMAGE + 16 * d + 12, true)]);
        }
      }
      if (Atomics.load(w32, at / 4) === seq1) {
        return s;
      }
    }
  };

  const text = (buffer, at) => {
    const m = new Uint8Array(buffer, at, 64);
    let end = 0;
    while (end < 64 && m[end]) {
      end++;
    }
    return decoder.decode(m.slice(0, end));
  };

  const screen = () => document.getElementById("screen");

  // ---- the screen's size: the page's, in CSS pixels, and its scale

  // What the screen should be now: [width, height, scale].
  const wantedSize = () => {
    let w, h;
    if (document.fullscreenElement === stage) {
      w = window.innerWidth;
      h = window.innerHeight;
    } else if (fixedSize) {
      w = Number(fixedSize[1]);
      h = Number(fixedSize[2]);
    } else {
      // All of the window's width (less the page's margins) and the rest of its height below the bar.
      const box = stage.getBoundingClientRect();
      const left = Math.max(0, box.left + window.scrollX);
      w = document.documentElement.clientWidth - 2 * left;
      h = window.innerHeight - (box.top + window.scrollY) - left;
    }
    const scale = scaleParam ? Math.max(1, Math.min(4, Math.round(scaleParam)))
      : Math.max(1, Math.min(3, Math.round(window.devicePixelRatio || 1)));
    const st = pageStruts();
    return [Math.max(320, Math.floor(w)), Math.max(240, Math.floor(h)), scale, panelHeight()].concat(st ? [st.top, st.left, st.right] : []);
  };

  let sized = null;		// [w, h, scale] last asked of the server
  let sizeTimer = 0;
  // Give #screen that size and ask the server for it (it fits its windows to it and tells the clients).
  const resize = () => {
    if (!active || !tableNow) {
      return;
    }
    const want = wantedSize();
    const sc = screen();
    sc.style.width = want[0] + "px";
    sc.style.height = want[1] + "px";
    sc.style.maxWidth = "none";
    if (sized && sized.join() === want.join()) {
      return;
    }
    sized = want;
    const { t } = tableNow;
    const buffer = os.memory.buffer;
    const v = new DataView(buffer), w32 = new Int32Array(buffer);
    v.setUint32(t + H_OUT_W, want[0], true);
    v.setUint32(t + H_OUT_H, want[1], true);
    v.setUint32(t + H_OUT_SCALE, want[2], true);
    v.setUint32(t + H_OUT_PANEL, want[3], true);
    // The other edges' strips (wlserver/PLASMA-PROTOCOL.md; a server that does not know them reads nothing there).
    const st = pageStruts() || { top: 0, left: 0, right: 0 };
    v.setUint32(t + H_OUT_STRUT_TOP, st.top, true);
    v.setUint32(t + H_OUT_STRUT_LEFT, st.left, true);
    v.setUint32(t + H_OUT_STRUT_RIGHT, st.right, true);
    Atomics.store(w32, (t + H_OUT_SERIAL) / 4, (v.getUint32(t + H_OUT_SERIAL, true) + 1) | 0);
    ringBell();
    say("display: asked for a screen of " + want[0] + "x" + want[1] + " at scale " + want[2]);
  };
  // (The window's size changes in many small steps while it is dragged: the last one counts.)
  const resizeSoon = () => {
    clearTimeout(sizeTimer);
    sizeTimer = setTimeout(resize, 150);
  };
  window.addEventListener("resize", resizeSoon);
  document.addEventListener("fullscreenchange", () => {
    resizeSoon();
    if (!document.fullscreenElement && navigator.keyboard && navigator.keyboard.unlock) {
      navigator.keyboard.unlock();
    }
  });

  // The screen as the server has it now (what the page draws by): [width, height, scale].
  const serverSize = (v, t) => {
    const w = v.getUint32(t + H_SCREEN_W, true) || 1024, h = v.getUint32(t + H_SCREEN_H, true) || 768;
    const s = table.version >= 3 ? v.getUint32(t + H_SCREEN_SCALE, true) || 1 : 1;
    return [w, h, s];
  };

  // Lay the canvas over #screen, with as many pixels as the server's screen has at its scale.
  let screenNow = [1024, 768, 1];
  const align = (v, t) => {
    const s = screen();
    const [w, h, scale] = screenNow = serverSize(v, t);
    if (canvas.width !== w * scale || canvas.height !== h * scale) {
      canvas.width = w * scale;
      canvas.height = h * scale;
      gl.viewport(0, 0, canvas.width, canvas.height);
    }
    gl.uniform2f(uniforms.screen, w, h);
    if (s) {
      const box = s.getBoundingClientRect();
      canvas.style.width = box.width + "px";
      canvas.style.height = box.height + "px";
    }
  };

  const stop = (why) => {
    if (active) {
      say("display: " + why + "; the frame buffer is the screen again");
    }
    active = false;
    table = null;
    sized = null;
    clearChrome();
    if (screen()) {
      const st = screen().style;
      st.cursor = "";
      st.width = "";
      st.height = "";
      st.maxWidth = "";
    }
    if (bar) {
      bar.style.display = "none";
    }
    if (panel) {
      panel.style.display = "none";
      launcher.style.display = "none";
      tasksShown = "";
    }
    if (document.fullscreenElement === stage) {
      document.exitFullscreen().catch(() => {});
    }
    cursorSerial = -1;
    guestSerial = -1;
    for (const s of surfaces.values()) {
      gl.deleteTexture(s.texture);
    }
    surfaces.clear();
    if (canvas) {
      canvas.style.display = "none";
    }
  };

  // The state the drawing below relies on, set again: a GPU executor on this context (gpu-exec.js, ?gpu=main)
  // leaves its own program, vertex array, frame buffer and blending behind.
  const restoreState = () => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.useProgram(program);
    gl.bindVertexArray(vao);
    gl.activeTexture(gl.TEXTURE0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.disable(gl.SCISSOR_TEST);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);
    gl.colorMask(true, true, true, true);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
  };

  // ---- one frame
  const frame = () => {
    if (!table || !os || !os.memory) {
      return;
    }
    const memory = os.memory;
    const buffer = memory.buffer;
    const v = new DataView(buffer);
    const w32 = new Int32Array(buffer);
    const t = table.address;
    const now = performance.now();
    if (t + table.header + table.slots * table.slotSize > buffer.byteLength || v.getUint32(t + H_MAGIC, true) !== MAGIC
        || v.getUint32(t + H_PID, true) !== table.pid) {
      stop("the window server's table is gone");
      return;
    }
    const beat = v.getUint32(t + H_HEARTBEAT, true);
    if (beat !== lastBeat) {
      lastBeat = beat;
      lastBeatAt = now;
    } else if (now - lastBeatAt > DEAD_MS) {
      stop("the window server stopped (no heartbeat for " + Math.round(now - lastBeatAt) + " ms)");
      return;
    }
    if (!active) {
      active = true;
      stats.startedAt = now;
      stats.lastReport = now;
      canvas.style.display = "block";
      tableNow = { v, w32, t };
      say("display: drawing windows from the table (" + table.slots + " slots)");
      if (bar) {
        bar.style.display = "";
      }
      resize();
      sendLayout();
    }
    tableNow = { v, w32, t };
    shellVersion = v.getUint32(t + H_SHELL_VERSION, true);
    {
      const native = shellParam || !!(v.getUint32(t + H_FLAGS, true) & FLAG_NATIVE_SHELL);
      if (native !== nativeShell) {
        nativeShell = native;
        say("display: " + (native ? "plasmashell is the desktop's shell: the page's panel and wallpaper are not drawn"
          : "the page's own panel and wallpaper"));
        sized = null;
        resizeSoon();	// (the strips the page keeps change)
      }
    }
    desktopNow = shellVersion ? Math.max(1, v.getUint32(t + H_DESKTOP_CURRENT, true)) : 1;
    restoreState();
    align(v, t);
    stats.frames++;
    Atomics.store(w32, (t + H_PAGE_FRAME) / 4, stats.frames);
    if (!document.hidden) {
      Atomics.store(w32, (t + H_PAGE_HIDDEN) / 4, 0);	// (shown: the page acknowledges its frames itself)
    }
    Atomics.store(w32, (t + H_PAGE_TIME) / 4, now & 0x7fffffff);

    // The slots: what is there, and what has to be uploaded.
    const base = t + table.header;
    const windows = [];
    let frameBytes = 0, frameFull = 0, frameRects = 0;
    for (let i = 0; i < table.slots; i++) {
      const at = base + i * table.slotSize;
      const s = readSlot(v, w32, at);
      let surface = surfaces.get(i);
      if (!s.id || !s.buffer || !s.bw || !s.bh) {
        if (surface) {
          retire(surface);
          surfaces.delete(i);
        }
        continue;
      }
      if (s.format === FORMAT_GPU) {
        // The GPU's texture, as it is: nothing to upload. (A slot that was a wl_shm window before lets its
        // texture go.)
        if (surface && surface.texture) {
          gl.deleteTexture(surface.texture);
        }
        const before = surface && surface.gpu && surface.id === s.id ? surface.present : -1;
        const res = s.buffer % 0x100000000, pid = Math.floor(s.buffer / 0x100000000);
        const gpu = window.rvGpu;
        const done = gpu ? gpu.done() : null;
        const serial = v.getUint32(at + S_GPU_SERIAL, true);
        // (until the client's rendering of this frame is done, the last frame drawn stays)
        const ready = done === null || ((done - serial) | 0) >= 0;
        const g = gpu && gpu.texture(pid, res, ready ? s.present : 0);
        if (!g) {
          surfaces.delete(i);
          continue;
        }
        surface = { id: s.id, texture: null, gpu: g, w: s.bw, h: s.bh, present: g.present || s.present, filter: gl.NEAREST };
        surfaces.set(i, surface);
        if (surface.present !== before && (!g.present || g.present === s.present)) {
          stats.gpuPresents = (stats.gpuPresents || 0) + 1;
          Atomics.store(w32, (at + S_UPLOADED) >> 2, s.present);
        }
        s.surface = surface;
        // (as for any window below: without its desktop a GPU window counts as on another one, never drawn)
        s.desktop = shellVersion ? s.desktopRaw : 1;
        s.shellFlags = shellVersion ? s.shellRaw : 0;
        windows.push(s);
        continue;
      }
      if (surface && (surface.id !== s.id || !surface.texture)) {
        retire(surface);
        surface = null;
      }
      if (!surface) {
        surface = { id: s.id, texture: newTexture(s.bw, s.bh), w: s.bw, h: s.bh, present: 0, filter: gl.NEAREST };
        surfaces.set(i, surface);
      }
      gl.bindTexture(gl.TEXTURE_2D, surface.texture);
      // Sharp where a buffer pixel is a whole number of the canvas's (a scale-2 buffer on a scale-2 screen, or a
      // scale-1 buffer doubled), smoothed where it is not (a viewport's destination size, say).
      const ratio = screenNow[2] * s.w / s.bw;
      const filter = Math.abs(ratio - Math.round(ratio)) < 0.01 ? gl.NEAREST : gl.LINEAR;
      if (filter !== surface.filter) {
        surface.filter = filter;
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
      }
      if (surface.w !== s.bw || surface.h !== s.bh) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, s.bw, s.bh, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        surface.w = s.bw;
        surface.h = s.bh;
        surface.present = 0;	// (everything again)
      }
      if (s.present !== surface.present) {
        const rects = surface.present === 0 || s.damage === null ? [[0, 0, s.bw, s.bh]] : s.damage;
        for (const r of rects) {
          const x = Math.max(0, r[0]), y = Math.max(0, r[1]);
          const w = Math.min(s.bw, r[0] + r[2]) - x, h = Math.min(s.bh, r[1] + r[3]) - y;
          if (w > 0 && h > 0) {
            frameBytes += upload(buffer, s.buffer, s.stride, x, y, w, h);
            frameRects++;
          }
        }
        frameFull += s.stride * s.bh;
        surface.present = s.present;
        stats.uploads++;
        Atomics.store(w32, (at + S_UPLOADED) / 4, s.present);
        ringBell();
      }
      s.surface = surface;
      s.desktop = shellVersion ? s.desktopRaw : 1;	// (1-based; 0 on all desktops)
      s.shellFlags = shellVersion ? s.shellRaw : 0;	// (1 above, 2 below, 4 shaded, 8 skip taskbar, 16 skip switcher, ...)
      windows.push(s);
    }
    stats.uploadBytes += frameBytes;
    stats.fullBytes += frameFull;
    stats.lastFrameBytes = frameBytes;
    stats.lastFrameFull = frameFull;
    stats.lastFrameRects = frameRects;
    stats.windows = windows.length;

    // Draw: the frame buffer at the bottom (surface 0), then the windows in their order.
    gl.clearColor(0.11, 0.17, 0.21, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    // (plasmashell's desktop, a layer surface of the background layer, covers the screen: no wallpaper of ours
    // under it; until it is there, ours)
    if (!(nativeShell && windows.some((s) => s.kind === KIND_LAYER && s.layer === 0 && !(s.flags & F_MINIMIZED)))) {
      drawWallpaper();
    }
    if (showConsole && fb && fb.pixels + fb.height * fb.stride <= buffer.byteLength) {
      if (!consoleSurface.texture || consoleSurface.w !== fb.width || consoleSurface.h !== fb.height) {
        if (consoleSurface.texture) {
          gl.deleteTexture(consoleSurface.texture);
        }
        consoleSurface.texture = newTexture(fb.width, fb.height);
        consoleSurface.w = fb.width;
        consoleSurface.h = fb.height;
      }
      gl.bindTexture(gl.TEXTURE_2D, consoleSurface.texture);
      stats.consoleBytes += upload(buffer, fb.pixels, fb.stride, 0, 0, fb.width, fb.height);
      gl.uniform1i(uniforms.swizzle, 0);	// (the kernel's buffer is R, G, B, X)
      gl.uniform1i(uniforms.flip, 0);
      gl.uniform1i(uniforms.opaque, 1);
      // Letterboxed: as big as fits, in the middle, its own shape kept.
      const k = Math.min(screenNow[0] / fb.width, screenNow[1] / fb.height);
      gl.uniform4f(uniforms.rect, (screenNow[0] - fb.width * k) / 2, (screenNow[1] - fb.height * k) / 2, fb.width * k, fb.height * k);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    windows.sort((a, b) => a.z - b.z);
    const byId = new Map();
    for (const s of windows) {
      byId.set(s.id, s);
    }
    fxBegin(windows, now);
    for (const s of windows) {
      const top = topOf(s, byId), f = top ? fx.cur.get(top.id) : null;
      if (hiddenBy(s, byId) && !(f && f.show)) {
        if (Atomics.exchange(w32, (s.at + S_SHOWN) / 4, s.present) !== s.present) {	// (a minimized window is not waited for)
          ringBell();
        }
        s.sx = s.x;
        s.sy = s.y;
        continue;
      }
      let x = s.x, y = s.y;
      if (s.kind === KIND_SUBSURFACE) {
        // Relative to the parent (and its parent, ...).
        for (let p = byId.get(s.parent); p; p = p.kind === KIND_SUBSURFACE ? byId.get(p.parent) : null) {
          x += p.x;
          y += p.y;
        }
      }
      s.sx = x;
      s.sy = y;
      const g = s.surface.gpu;
      gl.bindTexture(gl.TEXTURE_2D, g ? g.texture : s.surface.texture);
      // (formats 3, 4 and the GPU's RGBA textures are R, G, B, A; wl_shm's and the GPU's BGRA ones B, G, R, A)
      gl.uniform1i(uniforms.swizzle, g ? (g.bgra ? 1 : 0) : s.format === FORMAT_ABGR8888 || s.format === FORMAT_XBGR8888 ? 0 : 1);
      gl.uniform1i(uniforms.flip, g && g.flipY ? 1 : 0);
      const opaque = s.format === FORMAT_XRGB8888 || s.format === FORMAT_XBGR8888 || !!g;
      gl.uniform1i(uniforms.opaque, opaque ? 1 : 0);
      const r = f ? fxRect(f, x, y, s.w, s.h) : null;
      if (r) {
        gl.uniform1f(uniforms.fade, f.alpha);
        gl.uniform4f(uniforms.rect, r[0], r[1], r[2], r[3]);
      } else {
        gl.uniform4f(uniforms.rect, x, y, s.w, s.h);
      }
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      if (r) {
        gl.uniform1f(uniforms.fade, 1);
      }
      if (g && g.flipY) {
        gl.uniform1i(uniforms.flip, 0);	// (the ghosts and the chrome are drawn the right way up)
      }
      // (what a closing animation starts from, should the slot go)
      const tr = top ? geometryRect(top) : [x, y, s.w, s.h];
      s.surface.last = { rect: [x, y, s.w, s.h], center: [tr[0] + tr[2] / 2, tr[1] + tr[3] / 2], top: s.kind === KIND_TOPLEVEL, kind: s.kind,
                         opaque, when: now };
      // (a GPU window from a Worker shows the frame it has, which may be older than the slot's)
      const shown = g && g.present ? g.present : s.present;
      if (Atomics.exchange(w32, (s.at + S_SHOWN) / 4, shown) !== shown) {
        ringBell();
      }
    }
    drawGhosts(now);
    lastWindows = windows;
    chromeFrame(windows, buffer, t, v);
    embedFrame(windows, buffer, t, v);
    fxFrames(now);
    trackFocus(v.getUint32(t + H_FOCUS, true));
    barFrame(v, t);
    panelFrame(v, t);
    cursorFrame(v, buffer, t);
    clipboardFrame(v, w32, buffer, t);
    imeFrame(v, w32, buffer, t);
    flushOrders(v, w32, t);

    // A line of numbers every five seconds.
    stats.reportFrames++;
    stats.reportBytes += frameBytes;
    stats.reportFull += frameFull;
    if (now - stats.lastReport > 5000) {
      const dt = (now - stats.lastReport) / 1000;
      stats.fps = stats.reportFrames / dt;
      stats.perFrame = stats.reportBytes / stats.reportFrames;
      stats.perFrameFull = stats.reportFull / stats.reportFrames;
      stats.perFrameConsole = fb && showConsole ? fb.height * fb.stride : 0;
      say("display: " + stats.fps.toFixed(1) + " frames/s, " + windows.length + " windows, uploaded "
          + (stats.perFrame / 1024).toFixed(1) + " KiB/frame (whole windows would be " + (stats.perFrameFull / 1024).toFixed(1)
          + " KiB/frame); the frame buffer backdrop " + (stats.perFrameConsole / 1024).toFixed(0) + " KiB/frame");
      stats.lastReport = now;
      stats.reportFrames = 0;
      stats.reportBytes = 0;
      stats.reportFull = 0;
    }
  };
  let lastWindows = [];
  let tableNow = null;

  // The desktop's wallpaper (Plasma's, plasma.js): a texture drawn under the windows, covering the screen.
  const wallpaperTexture = { texture: null, serial: -1, w: 0, h: 0 };
  const drawWallpaper = () => {
    const T = theme();
    const wp = T && T.wallpaper(screenNow[0] * screenNow[2], screenNow[1] * screenNow[2]);
    if (!wp || !wp.bitmap) {
      return;
    }
    if (wallpaperTexture.serial !== wp.serial) {
      if (!wallpaperTexture.texture) {
        wallpaperTexture.texture = gl.createTexture();
      }
      gl.bindTexture(gl.TEXTURE_2D, wallpaperTexture.texture);
      gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
      gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
      gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, wp.bitmap);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      wallpaperTexture.serial = wp.serial;
      wallpaperTexture.w = wp.bitmap.width;
      wallpaperTexture.h = wp.bitmap.height;
    }
    gl.bindTexture(gl.TEXTURE_2D, wallpaperTexture.texture);
    gl.uniform1i(uniforms.swizzle, 0);
    gl.uniform1i(uniforms.opaque, 1);
    // Scaled and cropped (Plasma's default, FillMode 2), or scaled to fit (1), or stretched (0).
    const [W, H] = screenNow, iw = wallpaperTexture.w, ih = wallpaperTexture.h;
    if (wp.fill === 0) {
      gl.uniform4f(uniforms.rect, 0, 0, W, H);
    } else {
      const k = wp.fill === 1 ? Math.min(W / iw, H / ih) : Math.max(W / iw, H / ih);
      gl.uniform4f(uniforms.rect, (W - iw * k) / 2, (H - ih * k) / 2, iw * k, ih * k);
    }
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  };

  // A window's picture as it is now, in a canvas at most w x h (the switcher's thumbnails): its texture read back.
  let readFbo = null;
  const thumbnail = (id, w, h) => {
    let surface = null;
    for (const s of surfaces.values()) {
      if (s.id === id) {
        surface = s;
      }
    }
    if (!surface || !surface.w || !surface.h || !gl) {
      return null;
    }
    if (!readFbo) {
      readFbo = gl.createFramebuffer();
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, readFbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, surface.texture, 0);
    const pixels = new Uint8ClampedArray(surface.w * surface.h * 4);
    gl.readPixels(0, 0, surface.w, surface.h, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    for (let i = 0; i < pixels.length; i += 4) {
      const b = pixels[i];
      pixels[i] = pixels[i + 2];	// (the texture has the buffer's B, G, R, A)
      pixels[i + 2] = b;
      pixels[i + 3] = 255;
    }
    const full = document.createElement("canvas");
    full.width = surface.w;
    full.height = surface.h;
    full.getContext("2d").putImageData(new ImageData(pixels, surface.w, surface.h), 0, 0);
    const k = Math.min(1, w / surface.w, h / surface.h);
    const c = document.createElement("canvas");
    c.width = Math.max(1, Math.round(surface.w * k));
    c.height = Math.max(1, Math.round(surface.h * k));
    const ctx = c.getContext("2d");
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(full, 0, 0, c.width, c.height);
    return c;
  };

  // Is a slot hidden because its window (it, or the window it belongs to) is minimized, shaded (its title bar only:
  // the page draws that), or on another virtual desktop (PLASMA-PROTOCOL.md: desktop 0 is all of them)?
  let shellVersion = 0, desktopNow = 1;
  const hiddenBy = (s, byId) => {
    for (let p = s, n = 0; p && n < 16; n++) {
      if (p.flags & F_MINIMIZED) {
        return true;
      }
      if (shellVersion && p.kind === KIND_TOPLEVEL && ((p.shellFlags & 4) || (p.desktop !== 0 && p.desktop !== desktopNow))) {
        return true;
      }
      p = p.parent ? byId.get(p.parent) : null;
    }
    return false;
  };

  // ---- effects (KWin's, drawn by the page: kwinrc [Plugins], plasma.js): windows open and close with Scale (a
  // fade and a little growth), minimize into their task button (Squash), maximize and restore from where they
  // were, virtual desktops slide. Each animation is a transform of a window (scale about its middle, a shift, an
  // opacity) applied to its surfaces in the draw and to its frame in the DOM; AnimationDurationFactor scales the
  // times, 0 (and prefers-reduced-motion) turns them off. Nothing of it reaches the machine.
  const fx = { prev: new Map(), anims: new Map(), cur: new Map(), ghosts: [], closing: [], slide: null, lastDesktop: 0, sig: "" };
  const ease = (t) => 1 - Math.pow(1 - Math.min(1, Math.max(0, t)), 3);
  const lerp = (a, b, t) => a + (b - a) * t;
  const effectsOn = () => {
    const T = theme(), th = T && T.theme;
    return th && th.anim > 0 && th.effects ? { f: th.anim, e: th.effects } : null;
  };
  // A surface whose slot went away: kept for its window's closing animation, then its texture goes.
  const retire = (surface) => {
    if (!surface.texture) {
      return;	// (a GPU window's texture is the executor's)
    }
    const c = effectsOn(), last = surface.last;
    if (c && last && performance.now() - last.when < 100 && ((last.top && c.e.open) || (last.kind === KIND_POPUP && c.e.popups))) {
      fx.ghosts.push({ texture: surface.texture, rect: last.rect, center: last.center, opaque: last.opaque, t0: performance.now(),
                       dur: (last.kind === KIND_POPUP ? 120 : 200) * c.f, popup: last.kind === KIND_POPUP });
      return;
    }
    gl.deleteTexture(surface.texture);
  };
  const geometryRect = (t) => [t.x + t.geometry[0], t.y + t.geometry[1], t.geometry[2], t.geometry[3]];
  // Once per frame, before drawing: start the animations that what changed calls for, and work out every window's
  // transform now (fx.cur: id -> { sx, sy, tx, ty, alpha, show, center }).
  const fxBegin = (windows, now) => {
    const c = effectsOn();
    fx.cur.clear();
    const present = new Set();
    for (const t of windows) {
      if (t.kind !== KIND_TOPLEVEL) {
        continue;
      }
      present.add(t.id);
      const R = geometryRect(t), prev = fx.prev.get(t.id);
      if (c) {
        if (!prev && !(t.flags & F_MINIMIZED) && c.e.open) {
          fx.anims.set(t.id, { kind: "open", t0: now, dur: 200 * c.f });
        } else if (prev && !(prev.flags & F_MINIMIZED) && (t.flags & F_MINIMIZED) && c.e.minimize) {
          const T = theme(), target = (T && T.taskButtonRect(t.id)) || [screenNow[0] / 2 - 20, screenNow[1] - 40, 40, 40];
          fx.anims.set(t.id, { kind: "min", t0: now, dur: 250 * c.f, from: prev.rect, to: target });
        } else if (prev && (prev.flags & F_MINIMIZED) && !(t.flags & F_MINIMIZED) && c.e.minimize) {
          const T = theme(), target = (T && T.taskButtonRect(t.id)) || [screenNow[0] / 2 - 20, screenNow[1] - 40, 40, 40];
          fx.anims.set(t.id, { kind: "unmin", t0: now, dur: 250 * c.f, from: R, to: target });
        } else if (prev && ((prev.flags ^ t.flags) & F_MAXIMIZED) && c.e.maximize && !(t.flags & F_MINIMIZED)) {
          fx.anims.set(t.id, { kind: "max", t0: now, dur: 200 * c.f, from: prev.rect });
        }
      }
      fx.prev.set(t.id, { flags: t.flags, rect: (t.flags & F_MINIMIZED) && prev ? prev.rect : R });
    }
    for (const id of [...fx.prev.keys()]) {
      if (!present.has(id)) {
        fx.prev.delete(id);
        fx.anims.delete(id);
      }
    }
    // The desktop changed: the old one's windows go out one way, the new one's come in from the other.
    if (shellVersion && fx.lastDesktop && desktopNow !== fx.lastDesktop && c && c.e.slide) {
      fx.slide = { from: fx.lastDesktop, to: desktopNow, t0: now, dur: 300 * c.f, dir: desktopNow > fx.lastDesktop ? 1 : -1 };
    }
    fx.lastDesktop = desktopNow;
    if (fx.slide && now - fx.slide.t0 >= fx.slide.dur) {
      fx.slide = null;
    }
    for (const t of windows) {
      if (t.kind !== KIND_TOPLEVEL) {
        continue;
      }
      const R = geometryRect(t);
      const center = [R[0] + R[2] / 2, R[1] + R[3] / 2];
      let f = null;
      const a = fx.anims.get(t.id);
      if (a) {
        const p = ease((now - a.t0) / a.dur);
        if (now - a.t0 >= a.dur) {
          fx.anims.delete(t.id);
        } else if (a.kind === "open") {
          f = { sx: lerp(0.9, 1, p), sy: lerp(0.9, 1, p), tx: 0, ty: 0, alpha: p };
        } else if (a.kind === "min" || a.kind === "unmin") {
          const q = a.kind === "min" ? p : 1 - p, F = a.from, T = a.to;
          const fc = [F[0] + F[2] / 2, F[1] + F[3] / 2], tc = [T[0] + T[2] / 2, T[1] + T[3] / 2];
          f = { sx: lerp(F[2] / R[2], T[2] / R[2], q), sy: lerp(F[3] / R[3], T[3] / R[3], q), tx: lerp(fc[0], tc[0], q) - center[0],
                ty: lerp(fc[1], tc[1], q) - center[1], alpha: lerp(1, 0.2, q), show: true };
        } else if (a.kind === "max") {
          const F = a.from, fc = [F[0] + F[2] / 2, F[1] + F[3] / 2];
          f = { sx: lerp(F[2] / R[2], 1, p), sy: lerp(F[3] / R[3], 1, p), tx: lerp(fc[0] - center[0], 0, p), ty: lerp(fc[1] - center[1], 0, p), alpha: 1 };
        }
      }
      if (fx.slide && (t.desktop === fx.slide.from || t.desktop === fx.slide.to) && t.desktop !== 0) {
        const p = ease((now - fx.slide.t0) / fx.slide.dur), W = screenNow[0];
        const tx = t.desktop === fx.slide.from ? -fx.slide.dir * W * p : fx.slide.dir * W * (1 - p);
        f = f || { sx: 1, sy: 1, tx: 0, ty: 0, alpha: 1 };
        f.tx += tx;
        f.show = true;
      }
      if (f) {
        f.center = center;
        fx.cur.set(t.id, f);
      }
    }
    fx.sig = [...fx.cur.entries()].filter(([, f]) => f.show).map(([id]) => id).join(",");
  };
  // The toplevel a slot belongs to (itself, its parent, ...).
  const topOf = (s, byId) => {
    let p = s;
    for (let n = 0; p && p.kind !== KIND_TOPLEVEL && n < 16; n++) {
      p = byId.get(p.parent);
    }
    return p;
  };
  // A rectangle through a window's transform.
  const fxRect = (f, x, y, w, h) => [f.center[0] + (x - f.center[0]) * f.sx + f.tx, f.center[1] + (y - f.center[1]) * f.sy + f.ty, w * f.sx, h * f.sy];
  // The closing windows' last pictures, fading and shrinking; then their textures go.
  const drawGhosts = (now) => {
    for (let i = fx.ghosts.length - 1; i >= 0; i--) {
      const g = fx.ghosts[i], p = ease((now - g.t0) / g.dur);
      if (now - g.t0 >= g.dur) {
        gl.deleteTexture(g.texture);
        fx.ghosts.splice(i, 1);
        continue;
      }
      const k = g.popup ? 1 : lerp(1, 0.9, p);
      const r = fxRect({ center: g.center, sx: k, sy: k, tx: 0, ty: 0 }, ...g.rect);
      gl.bindTexture(gl.TEXTURE_2D, g.texture);
      gl.uniform1i(uniforms.swizzle, 1);
      gl.uniform1i(uniforms.opaque, g.opaque ? 1 : 0);
      gl.uniform1f(uniforms.fade, 1 - p);
      gl.uniform4f(uniforms.rect, r[0], r[1], r[2], r[3]);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    gl.uniform1f(uniforms.fade, 1);
  };
  // The frames go with their windows: the same transform in CSS (and closing frames fade out with their ghosts).
  const fxFrames = (now) => {
    for (const [id, f] of frames) {
      const t = fx.cur.get(id);
      const want = t && f.r ? "translate(" + t.tx.toFixed(1) + "px," + t.ty.toFixed(1) + "px) scale(" + t.sx.toFixed(4) + "," + t.sy.toFixed(4) + ")" : "";
      if (f.fxTransform !== want) {
        f.fxTransform = want;
        f.el.style.transformOrigin = t && f.r ? (t.center[0] - f.r[0]) + "px " + (t.center[1] - f.r[1]) + "px" : "";
        f.el.style.transform = want;
        f.el.style.opacity = t ? String(t.alpha) : "";
      }
    }
    for (let i = fx.closing.length - 1; i >= 0; i--) {
      const c = fx.closing[i], p = ease((now - c.t0) / c.dur);
      if (now - c.t0 >= c.dur) {
        c.el.remove();
        fx.closing.splice(i, 1);
        continue;
      }
      const k = lerp(1, 0.9, p);
      c.el.style.transformOrigin = c.origin;
      c.el.style.transform = "scale(" + k.toFixed(4) + ")";
      c.el.style.opacity = String(1 - p);
    }
  };

  // ---- the orders ring: what the page asks of a window

  const pendingOrders = [];
  // A move or a resize is sent once per frame at most: the newest of each kind for each window counts.
  const order = (op, id, a = 0, b = 0, c = 0, d = 0) => {
    if (op === ORDER_MOVE || op === ORDER_RESIZE) {
      const same = pendingOrders.findIndex((o) => o.op === op && o.id === id);
      if (same >= 0) {
        pendingOrders.splice(same, 1);
      }
    }
    pendingOrders.push({ op, id, a, b, c, d });
  };
  const flushOrders = (v, w32, t) => {
    while (pendingOrders.length) {
      const head = v.getUint32(t + H_ORDER_HEAD, true), tail = v.getUint32(t + H_ORDER_TAIL, true);
      if (((head - tail) >>> 0) >= ORDERS) {
        return;	// (the server has not read the ring yet: the rest next frame)
      }
      const o = pendingOrders.shift();
      const at = t + H_ORDERS + (head % ORDERS) * ORDER_SIZE;
      v.setUint32(at, o.op, true);
      v.setUint32(at + 4, o.id, true);
      v.setInt32(at + 8, o.a, true);
      v.setInt32(at + 12, o.b, true);
      v.setInt32(at + 16, o.c, true);
      v.setInt32(at + 20, o.d || 0, true);
      Atomics.store(w32, (t + H_ORDER_HEAD) / 4, (head + 1) | 0);
      ringBell();
    }
  };

  // ---- the frames around windows

  let chromeRoot = null, dock = null;
  const frames = new Map();	// window id -> { el, title, text, sig }
  let lastLayout = "";
  let drag = null;		// { kind: "move" | "resize", id, edges, x0, y0, wx, wy, w, h, scale }

  const screenScale = () => {
    const s = screen();
    return s ? s.getBoundingClientRect().width / screenNow[0] : 1;
  };

  const focusScreen = () => {
    const s = screen();
    if (s && document.activeElement !== s) {
      s.focus({ preventScroll: true });
    }
  };

  const setupChrome = () => {
    const link = document.createElement("link");
    link.rel = "stylesheet";
    link.href = "chrome.css";
    document.head.appendChild(link);
    if (!plainChrome) {
      const sheet = document.createElement("link");
      sheet.rel = "stylesheet";
      sheet.href = "breeze.css";
      document.head.appendChild(sheet);
      const script = document.createElement("script");
      script.src = "plasma.js";
      document.head.appendChild(script);
      // Plasma's splash screen over the stage from the moment a window server starts until Plasma's look is ready
      // (splash.js): the plain panel and background below are never seen when a desktop starts.
      const splash = document.createElement("script");
      splash.src = "splash.js";
      document.head.appendChild(splash);
    }
    // What plasma.js needs of the page.
    window.rvDisplayHost = {
      stage, ringBell, apps: () => apps, screenSize: () => screenNow.slice(), thumbnail,
      get chromeRoot() {
        return chromeRoot;
      },
      cursorsChanged: () => {
        cursorSerial = -1;
      },
      panelChanged: () => {
        panelShown = "";
      },
      // A new theme: frames made again, the panel drawn again, the screen asked for again (the panel's height).
      themeChanged: () => {
        for (const f of frames.values()) {
          f.el.remove();
        }
        frames.clear();
        lastLayout = "";
        tasksShown = "";
        cursorSerial = -1;
        if (launcher && launcher.style.display !== "none") {
          toggleLauncher(false);
        }
        resize();
      },
    };
    chromeRoot = document.createElement("div");
    chromeRoot.id = "rv-chrome";
    chromeRoot.style.display = "none";
    stage.appendChild(chromeRoot);
    dock = document.createElement("div");
    dock.id = "rv-dock";
    chromeRoot.appendChild(dock);
    window.addEventListener("pointermove", (event) => {
      if (!drag) {
        return;
      }
      const dx = Math.round((event.clientX - drag.x0) / drag.scale), dy = Math.round((event.clientY - drag.y0) / drag.scale);
      if (drag.kind === "move") {
        // A tiled window dragged away gets its size from before back (KWin does the same), under the pointer.
        const before = tiled.get(drag.id);
        if (before && Math.abs(dx) + Math.abs(dy) > 8) {
          tiled.delete(drag.id);
          order(ORDER_RESIZE, drag.id, 10, before[0], before[1]);
          order(ORDER_RESIZE_END, drag.id);
          drag.wx = drag.wx + Math.round((event.clientX - drag.x0) / drag.scale) - Math.round(before[0] / 2);
          drag.x0 = event.clientX;
          drag.y0 = event.clientY;
        }
        order(ORDER_MOVE, drag.id, drag.wx + dx, drag.wy + dy);
        if (!plainChrome && !drag.free) {
          quickTilePreview(event.clientX, event.clientY);
        }
      } else {
        let w = drag.w, h = drag.h;
        if (drag.edges & 8) {
          w += dx;
        } else if (drag.edges & 4) {
          w -= dx;
        }
        if (drag.edges & 2) {
          h += dy;
        } else if (drag.edges & 1) {
          h -= dy;
        }
        order(ORDER_RESIZE, drag.id, drag.edges, Math.max(1, w), Math.max(1, h));
      }
    });
    const end = () => {
      if (drag && drag.free) {
        return;	// (ended by a click: frameApi.startMove)
      }
      if (drag && drag.kind === "move" && tilePreview && tilePreview.tile) {
        quickTile(drag.id, tilePreview.tile);
      }
      hideTilePreview();
      if (drag && drag.kind === "resize") {
        order(ORDER_RESIZE_END, drag.id);
      }
      drag = null;
    };
    window.addEventListener("pointerup", end);
    window.addEventListener("pointercancel", end);
  };

  const clearChrome = () => {
    for (const f of frames.values()) {
      f.el.remove();
    }
    frames.clear();
    lastLayout = "";
    if (chromeRoot) {
      chromeRoot.style.display = "none";
      dock.textContent = "";
    }
  };

  // Where a decorated window's frame goes, in screen pixels: [x, y, w, h].
  const frameRect = (s, title, border) => {
    const g = s.geometry;
    return [s.sx + g[0] - border, s.sy + g[1] - title - border, g[2] + 2 * border, g[3] + title + 2 * border];
  };

  // The rectangles of `rect` that none of `holes` covers (disjoint), for a clip path.
  const subtract = (rect, holes) => {
    let parts = [rect];
    for (const h of holes) {
      const next = [];
      for (const r of parts) {
        const x0 = Math.max(r[0], h[0]), y0 = Math.max(r[1], h[1]);
        const x1 = Math.min(r[0] + r[2], h[0] + h[2]), y1 = Math.min(r[1] + r[3], h[1] + h[3]);
        if (x0 >= x1 || y0 >= y1) {
          next.push(r);
          continue;
        }
        if (r[1] < y0) {
          next.push([r[0], r[1], r[2], y0 - r[1]]);
        }
        if (y1 < r[1] + r[3]) {
          next.push([r[0], y1, r[2], r[1] + r[3] - y1]);
        }
        if (r[0] < x0) {
          next.push([r[0], y0, x0 - r[0], y1 - y0]);
        }
        if (x1 < r[0] + r[2]) {
          next.push([x1, y0, r[0] + r[2] - x1, y1 - y0]);
        }
      }
      parts = next;
    }
    return parts;
  };

  // What a theme's frame may do (plasma.js): orders by name or number, the shell's state, a move or resize by the
  // pointer without a button held (the window menu's Move and Resize: a click ends it, as in KWin).
  const frameOps = { close: ORDER_CLOSE, minimize: ORDER_MINIMIZE, maximize: ORDER_MAXIMIZE, fullscreen: ORDER_FULLSCREEN };
  const frameApi = {
    order: (what, id) => order(frameOps[what], id),
    raw: (op, id, a = 0, b = 0, c = 0, d = 0) => order(op, id, a, b, c, d),
    shell: () => shellState(),
    focusScreen: () => focusScreen(),
    startMove: (id, resize) => {
      const f = frames.get(id);
      if (!f || !f.s || (f.s.flags & F_MAXIMIZED)) {
        return;
      }
      order(ORDER_ACTIVATE, id);
      const g = f.s.geometry;
      drag = resize ? { kind: "resize", id, edges: 10, x0: lastPointer[0], y0: lastPointer[1], w: g[2], h: g[3], scale: screenScale(), free: true }
        : { kind: "move", id, x0: lastPointer[0], y0: lastPointer[1], wx: f.s.sx + g[0], wy: f.s.sy + g[1], scale: screenScale(), free: true };
    },
  };
  let lastPointer = [0, 0];
  window.addEventListener("pointermove", (event) => {
    lastPointer = [event.clientX, event.clientY];
  }, true);
  window.addEventListener("pointerdown", (event) => {
    if (drag && drag.free) {
      // (the window menu's Move or Resize ends with a click, which goes nowhere else)
      event.preventDefault();
      event.stopPropagation();
      if (drag.kind === "resize") {
        order(ORDER_RESIZE_END, drag.id);
      }
      drag = null;
    }
  }, true);

  // ---- quick tiling (KWin's: a window dragged by its title bar to the left or right edge takes that half of the
  // screen, to a corner a quarter, to the top edge all of it; dragged away it gets its size back). The screen less
  // the panels' strips; carried out with a move and a resize, which any server does, or the shell's tile order.
  const tiled = new Map();	// id -> [w, h] before
  let tilePreview = null;	// { el, tile }
  const workArea = () => {
    if (nativeShell && tableNow) {
      // (the server's usable area: the screen less plasmashell's panels' exclusive zones)
      const { v, t } = tableNow;
      return [v.getInt32(t + 1960, true), v.getInt32(t + 1964, true), v.getInt32(t + 1968, true), v.getInt32(t + 1972, true)];
    }
    const T = theme(), st = T ? T.panel.struts() : { top: 0, left: 0, right: 0, bottom: PANEL_HEIGHT };
    return [st.left, st.top, screenNow[0] - st.left - st.right, screenNow[1] - st.top - (T ? st.bottom : PANEL_HEIGHT)];
  };
  // KWin's tile numbers: 1 left, 2 right, 4 top, 8 bottom; 15 all (maximize).
  const tileRect = (tile) => {
    const [x, y, w, h] = workArea();
    if (tile === 15) {
      return [x, y, w, h];
    }
    let rx = x, ry = y, rw = w, rh = h;
    if (tile & 1) {
      rw = Math.floor(w / 2);
    } else if (tile & 2) {
      rx = x + Math.floor(w / 2);
      rw = w - Math.floor(w / 2);
    }
    if (tile & 4) {
      rh = Math.floor(h / 2);
    } else if (tile & 8) {
      ry = y + Math.floor(h / 2);
      rh = h - Math.floor(h / 2);
    }
    return [rx, ry, rw, rh];
  };
  const quickTilePreview = (clientX, clientY) => {
    const sc = screen();
    if (!sc || !tableNow) {
      return;
    }
    const box = sc.getBoundingClientRect(), k = box.width / screenNow[0];
    const px = (clientX - box.left) / k, py = (clientY - box.top) / k;
    const [ax, ay, aw, ah] = workArea();
    const edge = 3, corner = 40;
    let tile = 0;
    if (px <= ax + edge) {
      tile = 1;
    } else if (px >= ax + aw - edge) {
      tile = 2;
    } else if (py <= ay + edge) {
      tile = 15;
    }
    if (tile === 1 || tile === 2) {
      if (py <= ay + corner) {
        tile |= 4;
      } else if (py >= ay + ah - corner) {
        tile |= 8;
      }
    }
    if (!tile) {
      hideTilePreview();
      return;
    }
    if (!tilePreview) {
      const el = document.createElement("div");
      el.className = "rv-tile-preview";
      chromeRoot.appendChild(el);
      tilePreview = { el, tile: 0 };
    }
    if (tilePreview.tile !== tile) {
      tilePreview.tile = tile;
      const r = tileRect(tile);
      Object.assign(tilePreview.el.style, { left: r[0] + "px", top: r[1] + "px", width: r[2] + "px", height: r[3] + "px" });
    }
  };
  const hideTilePreview = () => {
    if (tilePreview) {
      tilePreview.el.remove();
      tilePreview = null;
    }
  };
  const quickTile = (id, tile) => {
    const f = frames.get(id);
    if (!f || !f.s) {
      return;
    }
    if (tile === 15) {
      order(ORDER_MAXIMIZE, id);
      return;
    }
    if (shellVersion) {
      order(ORDER_TILE, id, tile);	// (the server tiles within its work area and gives the size back later)
      return;
    }
    const titleH = tableNow.v.getUint32(tableNow.t + H_TITLE_HEIGHT, true);
    const T = theme(), b = T && T.theme.decoration.kind === "breeze" ? T.theme.decoration.border : tableNow.v.getUint32(tableNow.t + H_BORDER, true);
    const r = tileRect(tile);
    if (!tiled.has(id)) {
      tiled.set(id, [f.s.geometry[2], f.s.geometry[3]]);
    }
    order(ORDER_MOVE, id, r[0] + b, r[1] + titleH);
    order(ORDER_RESIZE, id, 10, Math.max(100, r[2] - 2 * b), Math.max(60, r[3] - titleH - b));
    order(ORDER_RESIZE_END, id);
  };

  const makeFrame = (id) => {
    const el = document.createElement("div");
    el.className = "rv-frame";
    const title = document.createElement("div");
    title.className = "rv-title";
    const text = document.createElement("span");
    text.className = "rv-title-text";
    title.appendChild(text);
    const button = (cls, label, what, op) => {
      const b = document.createElement("button");
      b.className = "rv-btn " + cls;
      b.textContent = label;
      b.title = what;
      b.tabIndex = -1;
      b.addEventListener("pointerdown", (event) => {
        event.stopPropagation();
        event.preventDefault();
        focusScreen();
      });
      b.addEventListener("click", (event) => {
        event.stopPropagation();
        order(op, id);
      });
      title.appendChild(b);
    };
    button("rv-min", "\u2013", "Minimize", ORDER_MINIMIZE);
    button("rv-max", "\u25a1", "Maximize", ORDER_MAXIMIZE);
    button("rv-close", "\u2715", "Close", ORDER_CLOSE);
    title.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) {
        return;
      }
      event.preventDefault();
      focusScreen();
      order(ORDER_ACTIVATE, id);
      const f = frames.get(id);
      if (!f || (f.s.flags & F_MAXIMIZED)) {
        return;
      }
      title.setPointerCapture(event.pointerId);
      drag = { kind: "move", id, x0: event.clientX, y0: event.clientY, wx: f.s.sx + f.s.geometry[0],
               wy: f.s.sy + f.s.geometry[1], scale: screenScale() };
    });
    title.addEventListener("dblclick", () => {
      // (kwinrc [Windows] TitlebarDoubleClickCommand, with the theme: Maximize, Minimize, Close, or nothing)
      const T = theme(), what = T ? T.theme.decoration.doubleClick : "Maximize";
      const op = { Maximize: ORDER_MAXIMIZE, Minimize: ORDER_MINIMIZE, Close: ORDER_CLOSE, "Maximize (vertical only)": ORDER_MAXIMIZE,
                   "Maximize (horizontal only)": ORDER_MAXIMIZE }[what];
      if (op) {
        order(op, id);
      }
    });
    el.appendChild(title);
    const edges = {};
    for (const [name, value] of [["n", 1], ["s", 2], ["w", 4], ["e", 8], ["nw", 5], ["ne", 9], ["sw", 6], ["se", 10]]) {
      const e = document.createElement("div");
      e.className = "rv-edge rv-" + name;
      e.addEventListener("pointerdown", (event) => {
        if (event.button !== 0) {
          return;
        }
        event.preventDefault();
        focusScreen();
        order(ORDER_ACTIVATE, id);
        const f = frames.get(id);
        if (!f || (f.s.flags & F_MAXIMIZED)) {
          return;
        }
        e.setPointerCapture(event.pointerId);
        drag = { kind: "resize", id, edges: value, x0: event.clientX, y0: event.clientY, w: f.s.geometry[2],
                 h: f.s.geometry[3], scale: screenScale() };
      });
      el.appendChild(e);
      edges[name] = e;
    }
    chromeRoot.insertBefore(el, dock);
    const f = { el, title, text, edges, sig: "", s: null, theme: 0 };
    const T = theme();
    if (T) {
      T.decorateFrame(f, id, frameApi);
      title.addEventListener("contextmenu", (event) => {
        event.preventDefault();
        T.windowMenu(f, id, frameApi, null, [event.clientX, event.clientY]);
      });
    }
    return f;
  };

  // What covers a window: everything above it (windows with their frames, popups, other surfaces), as rectangles
  // in screen pixels.
  const holesAbove = (s, windows, byId, titleH, border, T) => {
    const holes = [];
    for (const o of windows) {
      if (o.z <= s.z || hiddenBy(o, byId)) {
        continue;
      }
      if (o.kind === KIND_TOPLEVEL && (o.flags & F_DECORATED)) {
        // (with a theme, the part of its frame that shows: a transparent band to resize by lets what is
        // under it show through, the frame of a window below included)
        const of = frames.get(o.id), fr = frameRect(o, titleH, border);
        holes.push(T && of && of.visible ? [fr[0] + of.visible[0], fr[1] + of.visible[1], of.visible[2], of.visible[3]] : fr);
      } else {
        holes.push([o.sx, o.sy, o.w, o.h]);
      }
    }
    return holes;
  };
  // An element that shows `vis` (placed at r's corner) cut to what the holes leave of it.
  const clipBy = (st, vis, r, holes) => {
    const parts = subtract(vis, holes);
    let clip;
    if (parts.length === 1 && parts[0] === vis) {
      clip = "";
    } else if (!parts.length) {
      clip = "inset(50%)";
    } else {
      clip = "path('" + parts.map((p) => "M" + (p[0] - r[0]) + " " + (p[1] - r[1]) + "h" + p[2] + "v" + p[3] + "h" + -p[2] + "z").join("") + "')";
    }
    if (st.clipPath !== clip) {
      st.clipPath = clip;
    }
  };

  const chromeFrame = (windows, buffer, t, v) => {
    if (!chromeRoot) {
      return;
    }
    const titleH = v.getUint32(t + H_TITLE_HEIGHT, true), border = v.getUint32(t + H_BORDER, true);
    const focus = v.getUint32(t + H_FOCUS, true);
    // Lay the chrome's box over #screen, at the screen's scale.
    const sc = screen();
    if (sc) {
      const box = sc.getBoundingClientRect();
      const k = box.width / screenNow[0];
      const st = chromeRoot.style;
      st.display = "block";
      st.left = "0px";
      st.top = "0px";
      st.width = screenNow[0] + "px";
      st.height = screenNow[1] + "px";
      st.transform = "scale(" + k + ")";
    }
    const byId = new Map(windows.map((s) => [s.id, s]));
    const decorated = [], minimized = [];
    for (const s of windows) {
      if (s.kind !== KIND_TOPLEVEL) {
        continue;
      }
      const shown = fx.cur.get(s.id);
      if ((s.flags & F_MINIMIZED) && !(shown && shown.show)) {
        minimized.push(s);
      } else if (shellVersion && s.desktop !== 0 && s.desktop !== desktopNow && !(shown && shown.show)) {
        continue;	// (on another desktop)
      } else if (s.flags & F_DECORATED) {
        if (shellVersion && (s.shellFlags & 4)) {
          s.geometry = [s.geometry[0], s.geometry[1], s.geometry[2], 0];	// (shaded: the frame is its title bar)
        }
        decorated.push(s);
      }
    }
    // What covers what: everything above a frame (windows with their frames, popups, other surfaces).
    const titles = new Map(decorated.map((s) => [s.id, text(buffer, s.at + S_TITLE)]));
    const T = theme();
    const appIds = T ? new Map(decorated.map((s) => [s.id, text(buffer, s.at + S_APP_ID)])) : null;
    const layout = windows.map((s) => [s.id, s.kind, s.sx, s.sy, s.w, s.h, s.z, s.flags, s.geometry.join(","), s.desktop, s.shellFlags].join(" ")).join(";")
      + "|" + desktopNow + "|" + fx.sig
      + "|" + focus + "|" + [...titles.values()].join("\u0001") + "|" + titleH + "," + border
      + (T ? "|" + T.serial + "|" + [...appIds.values()].join("\u0001") : "");
    if (layout === lastLayout) {
      return;
    }
    lastLayout = layout;
    const seen = new Set();
    for (const s of decorated) {
      seen.add(s.id);
      let f = frames.get(s.id);
      if (f && (T ? f.theme !== T.frameKey(shellVersion) : f.theme)) {
        f.el.remove();	// (made for another theme)
        f = null;
      }
      if (!f) {
        f = makeFrame(s.id);
        frames.set(s.id, f);
      }
      f.s = s;
      const r = frameRect(s, titleH, border);
      const st = f.el.style;
      st.left = r[0] + "px";
      st.top = r[1] + "px";
      st.width = r[2] + "px";
      st.height = r[3] + "px";
      st.zIndex = String(2 * s.z + 1);	// (2 * z + 2 is for a page shown in the window: embedFrame)
      f.el.classList.toggle("rv-active", focus === s.id);
      const ts = f.title.style;
      ts.left = border + "px";
      ts.top = border + "px";
      ts.width = s.geometry[2] + "px";
      ts.height = titleH + "px";
      const label = titles.get(s.id) || " ";
      if (f.text.textContent !== label) {
        f.text.textContent = label;
      }
      const c = Math.max(border, 10);	// (corners are easier to catch than the border is)
      const place = (e, x, y, w, h) => {
        e.style.left = x + "px";
        e.style.top = y + "px";
        e.style.width = w + "px";
        e.style.height = h + "px";
      };
      place(f.edges.n, 0, 0, r[2], border);
      place(f.edges.s, 0, r[3] - border, r[2], border);
      place(f.edges.w, 0, border, border, r[3] - 2 * border);
      place(f.edges.e, r[2] - border, border, border, r[3] - 2 * border);
      place(f.edges.nw, 0, 0, c, c);
      place(f.edges.ne, r[2] - c, 0, c, c);
      place(f.edges.sw, 0, r[3] - c, c, c);
      place(f.edges.se, r[2] - c, r[3] - c, c, c);
      // The theme's decoration, and how far it reaches out of the frame (a shadow): clipped as the frame is.
      const reach = T ? T.layoutFrame(f, s, { active: focus === s.id, titleH, border, appId: appIds.get(s.id), shellFlags: s.shellFlags,
                                              desktop: s.desktop }) : [0, 0, 0, 0];
      const vis = [r[0] - reach[0], r[1] - reach[1], r[2] + reach[0] + reach[2], r[3] + reach[1] + reach[3]];
      f.vis = vis;
      f.r = r;
    }
    // Clipped by what is above it (once every frame has been placed: what shows of a frame above is known then).
    for (const s of decorated) {
      const f = frames.get(s.id);
      clipBy(f.el.style, f.vis, f.r, holesAbove(s, windows, byId, titleH, border, T));
    }
    for (const [id, f] of frames) {
      if (!seen.has(id)) {
        // A window that closed: its frame fades out with its last picture (effects); else it just goes.
        const c = effectsOn();
        if (c && c.e.open && !byId.has(id) && f.r) {
          const g = f.s ? f.s.geometry : [0, 0, f.r[2], f.r[3]];
          const cx = f.s ? f.s.sx + g[0] + g[2] / 2 : f.r[0] + f.r[2] / 2, cy = f.s ? f.s.sy + g[1] + g[3] / 2 : f.r[1] + f.r[3] / 2;
          f.el.style.pointerEvents = "none";
          for (const e of f.el.querySelectorAll("*")) {
            e.style.pointerEvents = "none";
          }
          fx.closing.push({ el: f.el, t0: performance.now(), dur: 200 * c.f, origin: (cx - f.r[0]) + "px " + (cy - f.r[1]) + "px" });
        } else {
          f.el.remove();
        }
        frames.delete(id);
      }
    }
    // (Minimized windows are in the panel's task list.)
  };

  // ---- pages shown in windows (embed): a program of the machine makes a window whose app id is
  // "rvembed-TOKEN.NAME" and asks the page (on a channel of its own: blenderweb.js) to show a page there; the page
  // registers the element (an iframe) with rvDisplay.embed(TOKEN, element, { focus, blur }) and it is laid over the
  // window's geometry here, every frame: above the window's own frame, under the windows above it (cut to what
  // they leave), hidden with the window. The window is the server's like any other (moved, resized, stacked,
  // minimized, switched to, closed by the frame and the panel); the element is never moved in the DOM once in
  // (an iframe that is moved is loaded again). The keyboard: the element is told when its window gets the
  // keyboard (focus) and loses it (blur).
  const embeds = new Map();	// token -> { el, focus, blur, id, focused }
  const EMBED_PREFIX = "rvembed-";
  const embedToken = (appId) => (appId.startsWith(EMBED_PREFIX) ? appId.slice(EMBED_PREFIX.length).split(".")[0] : null);
  const embedFrame = (windows, buffer, t, v) => {
    if (!embeds.size || !chromeRoot) {
      return;
    }
    const titleH = v.getUint32(t + H_TITLE_HEIGHT, true), border = v.getUint32(t + H_BORDER, true);
    const focus = v.getUint32(t + H_FOCUS, true);
    const byId = new Map(windows.map((s) => [s.id, s]));
    const T = theme();
    const seen = new Set();
    for (const s of windows) {
      if (s.kind !== KIND_TOPLEVEL) {
        continue;
      }
      const token = embedToken(text(buffer, s.at + S_APP_ID));
      const e = token && embeds.get(token);
      if (!e) {
        continue;
      }
      seen.add(token);
      e.id = s.id;
      if (!e.el.parentNode) {
        e.el.style.position = "absolute";
        e.el.style.pointerEvents = "auto";
        chromeRoot.insertBefore(e.el, dock);
      }
      const g = s.geometry, st = e.el.style;
      const shaded = shellVersion && (s.shellFlags & 4);
      const hidden = hiddenBy(s, byId) || shaded || !g[2] || !g[3];
      const r = [s.sx + g[0], s.sy + g[1], g[2], g[3]];
      const place = [r[0] + "px", r[1] + "px", r[2] + "px", r[3] + "px", String(2 * s.z + 2), hidden ? "hidden" : "visible"];
      if (e.placed !== place.join(" ")) {
        e.placed = place.join(" ");
        [st.left, st.top, st.width, st.height, st.zIndex, st.visibility] = place;
      }
      if (!hidden) {
        clipBy(st, r, r, holesAbove(s, windows, byId, titleH, border, T));
      }
      const has = focus === s.id && !hidden;
      if (has !== e.focused) {
        e.focused = has;
        try {
          (has ? e.focus : e.blur)();
        } catch (error) {
          say("display: embed " + token + ": " + error);
        }
      }
    }
    for (const [token, e] of embeds) {
      if (!seen.has(token) && e.id) {
        e.id = 0;	// (its window is gone, or not yet there)
        e.placed = "";
        e.el.style.visibility = "hidden";
        if (e.focused) {
          e.focused = false;
          e.blur();
        }
      }
    }
  };
  const embedId = (token) => {
    const e = embeds.get(token);
    return e ? e.id : 0;
  };

  // ---- the pointer's picture

  let cursorSerial = -1;
  const cursorFrame = (v, buffer, t) => {
    // (While windows drag something between them, no client has the pointer: the page shows a drag.)
    const dragging = table.version >= 3 ? v.getUint32(t + H_DRAG, true) : 0;
    const serial = v.getUint32(t + H_CURSOR_SERIAL, true) * 2 + (dragging ? 1 : 0);
    if (serial === cursorSerial) {
      return;
    }
    cursorSerial = serial;
    const kind = v.getUint32(t + H_CURSOR_KIND, true);
    const sc = screen();
    if (!sc) {
      return;
    }
    const T = theme();
    const named = (name) => (T ? T.cursor(name) : name);
    if (dragging) {
      sc.style.cursor = named("copy");
    } else if (kind === 0) {
      sc.style.cursor = "none";
    } else if (kind === 1) {
      sc.style.cursor = named(CURSORS[v.getUint32(t + H_CURSOR_SHAPE, true)] || "default");
    } else if (kind === 2) {
      const w = v.getUint32(t + H_CURSOR_W, true), h = v.getUint32(t + H_CURSOR_H, true);
      const stride = v.getUint32(t + H_CURSOR_STRIDE, true);
      const at = v.getUint32(t + H_CURSOR_LO, true) + v.getUint32(t + H_CURSOR_HI, true) * 0x100000000;
      if (!w || !h || w > 256 || h > 256 || at + stride * h > buffer.byteLength) {
        sc.style.cursor = "default";
        return;
      }
      const c = document.createElement("canvas");
      c.width = w;
      c.height = h;
      const ctx = c.getContext("2d");
      const image = ctx.createImageData(w, h);
      const src = new Uint8Array(buffer, at, stride * h);
      for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
          const i = y * stride + x * 4, o = (y * w + x) * 4;
          const a = src[i + 3];
          // (premultiplied B, G, R, A to straight R, G, B, A)
          image.data[o] = a ? Math.min(255, src[i + 2] * 255 / a) : 0;
          image.data[o + 1] = a ? Math.min(255, src[i + 1] * 255 / a) : 0;
          image.data[o + 2] = a ? Math.min(255, src[i] * 255 / a) : 0;
          image.data[o + 3] = a;
        }
      }
      ctx.putImageData(image, 0, 0);
      const hx = Math.max(0, Math.min(w - 1, v.getInt32(t + H_CURSOR_HOT_X, true)));
      const hy = Math.max(0, Math.min(h - 1, v.getInt32(t + H_CURSOR_HOT_Y, true)));
      sc.style.cursor = "url(" + c.toDataURL("image/png") + ") " + hx + " " + hy + ", default";
    }
  };

  // ---- the clipboard

  const clipboard = { fromGuest: 0, toHost: 0, toHostFailed: 0, fromHost: 0, lastError: "", lastGuestText: null,
                      lastHostText: null, pendingHostWrite: null };
  let guestSerial = -1;
  const encoder = new TextEncoder();
  // Which is newer at a paste, the guest's clipboard or the host's? The page cannot see the host's change, but
  // it can only change while the person is somewhere else: so the host's counts as new since the page last
  // got the focus back (or was loaded), the guest's since a guest program last took the clipboard (any types:
  // files from Dolphin, an image; the server counts them, guest_sel_serial).
  let guestSel = -1, guestSelAt = -Infinity, hostMaybeAt = performance.now();
  const hostMayHaveChanged = () => {
    hostMaybeAt = performance.now();
  };
  window.addEventListener("focus", hostMayHaveChanged);
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) {
      hostMayHaveChanged();
    }
  });

  const writeHost = (text) => {
    if (!navigator.clipboard || !navigator.clipboard.writeText) {
      clipboard.lastError = "no navigator.clipboard here (not a secure context?)";
      return;
    }
    navigator.clipboard.writeText(text).then(() => {
      clipboard.toHost++;
      if (clipboard.pendingHostWrite === text) {
        clipboard.pendingHostWrite = null;
      }
    }, (error) => {
      // (No focus, or no user activation where the browser wants one: tried again on the next key or click.)
      clipboard.toHostFailed++;
      clipboard.lastError = String(error && error.message || error);
      clipboard.pendingHostWrite = text;
    });
  };

  const clipboardFrame = (v, w32, buffer, t) => {
    const sel = v.getUint32(t + H_GUEST_SEL_SERIAL, true);
    if (sel !== guestSel) {
      if (guestSel !== -1) {
        guestSelAt = performance.now();
      }
      guestSel = sel;
    }
    const serial = v.getUint32(t + H_GUEST_CLIP_SERIAL, true);
    if (guestSerial === -1) {
      guestSerial = serial;	// (what was there before this page looked is not news)
      return;
    }
    if (serial === guestSerial) {
      return;
    }
    guestSerial = serial;
    const at = v.getUint32(t + H_GUEST_CLIP_LO, true) + v.getUint32(t + H_GUEST_CLIP_HI, true) * 0x100000000;
    const len = v.getUint32(t + H_GUEST_CLIP_LEN, true);
    if (at + len > buffer.byteLength) {
      return;
    }
    const value = decoder.decode(new Uint8Array(buffer, at, len).slice());
    clipboard.fromGuest++;
    clipboard.lastGuestText = value;
    writeHost(value);
  };

  // Ctrl-V, Ctrl-Shift-V, Cmd-V on the screen: the host's text to the guest first, then the key.
  // (The keys themselves: see "the keyboard" below.)

  const giveHost = (text) => new Promise((resolve) => {
    if (!active || !tableNow || text === null || text === undefined || text === clipboard.lastGuestText) {
      resolve(false);	// (nothing new: what the guest has stays, with all its types)
      return;
    }
    if (guestSelAt > hostMaybeAt) {
      // A guest program copied something since the person was last away from the page: that is what they
      // mean to paste (files copied in Dolphin were replaced by the host's old text, and pasted nothing).
      clipboard.keptGuest = (clipboard.keptGuest || 0) + 1;
      resolve(false);
      return;
    }
    const { t } = tableNow;
    const buffer = os.memory.buffer;
    const v = new DataView(buffer), w32 = new Int32Array(buffer);
    const at = v.getUint32(t + H_HOST_CLIP_LO, true) + v.getUint32(t + H_HOST_CLIP_HI, true) * 0x100000000;
    const cap = v.getUint32(t + H_HOST_CLIP_CAP, true);
    let raw = encoder.encode(text);
    if (raw.length > cap) {
      raw = raw.subarray(0, cap);
    }
    new Uint8Array(buffer).set(raw, at);
    const serial = (v.getUint32(t + H_HOST_CLIP_SERIAL, true) + 1) >>> 0;
    Atomics.store(w32, (t + H_HOST_CLIP_LEN) / 4, raw.length);
    Atomics.store(w32, (t + H_HOST_CLIP_SERIAL) / 4, serial);
    ringBell();
    clipboard.lastHostText = text;
    clipboard.fromHost++;
    const began = performance.now();
    const wait = () => {
      if ((new Uint32Array(os.memory.buffer))[(t + H_HOST_CLIP_ACK) / 4] === serial || performance.now() - began > 1000) {
        resolve(true);
        return;
      }
      setTimeout(wait, 4);
    };
    wait();
  });

  const setupClipboard = () => {
    window.addEventListener("pointerdown", () => {
      if (clipboard.pendingHostWrite !== null) {
        writeHost(clipboard.pendingHostWrite);
      }
    }, true);
    // Where the pointer is over the screen, for the table (the server has it from the kernel; this is for others).
    document.addEventListener("mousemove", (event) => {
      const sc = screen();
      if (!active || !tableNow || event.target !== sc) {
        return;
      }
      const box = sc.getBoundingClientRect();
      const w32 = new Int32Array(os.memory.buffer);
      Atomics.store(w32, (tableNow.t + H_POINTER_X) / 4, Math.round((event.clientX - box.left) / box.width * screenNow[0]));
      Atomics.store(w32, (tableNow.t + H_POINTER_Y) / 4, Math.round((event.clientY - box.top) / box.height * screenNow[1]));
    });
  };

  // ---- the keyboard
  //
  // While a window server runs, the keys on the screen go through here instead of the runtime's handler
  // (which knows nothing of windows): they are physical keys, evdev codes from KeyboardEvent.code, and the
  // layout is the keymap the server gives the clients (the page names it: the bar, ?kbd=, or a guess from
  // navigator.keyboard.getLayoutMap()). On the way, the page takes the keys that are its own:
  //   Alt-Tab, Alt-Shift-Tab, Alt-` and Ctrl-Alt-Tab: the window switcher (letting go of Alt picks);
  //   Alt-F4: close the active window; Alt-Shift-O: the page of the browser in front to the host's browser;
  //   Super (or Cmd) pressed and let go alone: the list of windows.
  // On a Mac, Cmd acts as Ctrl for the usual shortcuts (copy, paste, cut, undo, select all, find, new, close,
  // save, open, print, reload, redo), and as Ctrl-Shift for those of them a terminal has (Konsole copies with
  // Ctrl-Shift-C: Ctrl-C would interrupt); any other key with Cmd is Super with it.
  // A paste (Ctrl-V, Ctrl-Shift-V, Cmd-V) gives the server the host's clipboard text first (the clipboard).

  // What the kernel calls the keys (linux/input-event-codes.h), by the browser's names (as the runtime has them).
  const KEY_CODES = {
    Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7, Digit7: 8, Digit8: 9, Digit9: 10,
    Digit0: 11, Minus: 12, Equal: 13, Backspace: 14, Tab: 15, KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20,
    KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24, KeyP: 25, BracketLeft: 26, BracketRight: 27, Enter: 28, ControlLeft: 29,
    KeyA: 30, KeyS: 31, KeyD: 32, KeyF: 33, KeyG: 34, KeyH: 35, KeyJ: 36, KeyK: 37, KeyL: 38, Semicolon: 39,
    Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43, KeyZ: 44, KeyX: 45, KeyC: 46, KeyV: 47, KeyB: 48,
    KeyN: 49, KeyM: 50, Comma: 51, Period: 52, Slash: 53, ShiftRight: 54, NumpadMultiply: 55, AltLeft: 56,
    Space: 57, CapsLock: 58, F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65, F8: 66, F9: 67, F10: 68,
    NumLock: 69, ScrollLock: 70, Numpad7: 71, Numpad8: 72, Numpad9: 73, NumpadSubtract: 74, Numpad4: 75,
    Numpad5: 76, Numpad6: 77, NumpadAdd: 78, Numpad1: 79, Numpad2: 80, Numpad3: 81, Numpad0: 82,
    NumpadDecimal: 83, IntlBackslash: 86, F11: 87, F12: 88, IntlRo: 89, NumpadEnter: 96, ControlRight: 97,
    NumpadDivide: 98, PrintScreen: 99, AltRight: 100, Home: 102, ArrowUp: 103, PageUp: 104, ArrowLeft: 105,
    ArrowRight: 106, End: 107, ArrowDown: 108, PageDown: 109, Insert: 110, Delete: 111, NumpadEqual: 117,
    Pause: 119, IntlYen: 124, MetaLeft: 125, MetaRight: 126, ContextMenu: 127, Lang1: 122, Lang2: 123,
  };
  const KEY_V = 47, CTRL = 97, SHIFT = 54;	// (Cmd's stand-ins are the right-hand Ctrl and Shift)
  const CMD_KEYS = new Set(["KeyC", "KeyV", "KeyX", "KeyZ", "KeyA", "KeyF", "KeyT", "KeyW", "KeyN", "KeyS", "KeyO",
                            "KeyP", "KeyR", "KeyY", "KeyG"]);
  const TERMINAL_KEYS = new Set(["KeyC", "KeyV", "KeyT", "KeyW", "KeyN", "KeyF"]);
  const TERMINALS = /konsole|terminal|foot|xterm|alacritty|kitty|wezterm|yakuake|terminator|tilix|urxvt/i;

  const guestDown = new Set();	// evdev codes the guest has been told are down
  let queue = null;		// while a paste waits for the host's text: key events held back, in order
  const send = (code, down) => {
    if (down) {
      guestDown.add(code);
    } else if (!guestDown.delete(code)) {
      return;	// (it was let go for the guest already, e.g. Alt when the switcher came up)
    }
    if (os && os.screen_input) {
      os.screen_input(0, code, down ? 1 : 0);
    }
  };
  const emit = (code, down) => {
    if (queue) {
      queue.push([code, down]);
    } else {
      send(code, down);
    }
  };
  const releaseAll = () => {
    queue = null;
    for (const code of [...guestDown]) {
      send(code, false);
    }
  };

  const phys = new Set();	// the browser's codes of the keys down on the screen
  let meta = null;		// Cmd/Super held: { code, sent (to the guest as Super), used (with another key) }
  const remapped = new Map();	// a key sent with Cmd's stand-ins: code -> { shift }
  const swallowed = new Set();	// keys the page took: their keyup goes nowhere either

  const focusedApp = () => {
    if (!tableNow || !os) {
      return "";
    }
    const focus = tableNow.v.getUint32(tableNow.t + H_FOCUS, true);
    const s = lastWindows.find((w) => w.id === focus);
    return s ? text(os.memory.buffer, s.at + S_APP_ID) + " " + text(os.memory.buffer, s.at + S_TITLE) : "";
  };

  // A key the guest is to have: V with Ctrl or Super is a paste, which waits for the host's text.
  const forward = (event, code) => {
    const pasteKey = code === KEY_V && (guestDown.has(29) || guestDown.has(CTRL) || guestDown.has(125) || guestDown.has(126));
    if (pasteKey && !queue) {
      startPaste();	// (and no preventDefault: the browser is to make a paste event of this key)
      return;
    }
    event.preventDefault();
    emit(code, true);
  };

  const onKeyDown = (event) => {
    if (overview && active) {
      // The Overview has the keyboard: Escape or Meta+W end it, the rest goes to its search field (which may
      // have lost the focus to the screen, as a click on the page can do).
      if (event.code === "Escape" || (event.code === "KeyW" && (event.metaKey || phys.has("MetaLeft") || phys.has("MetaRight")))) {
        event.preventDefault();
        event.stopImmediatePropagation();
        toggleOverview();
        return;
      }
      const search = overview.querySelector(".rv-ov-search");
      if (search && event.target !== search) {
        search.focus({ preventScroll: true });
      }
      return;
    }
    if (!active || !os || !(event.target === screen() || (ime.area && event.target === ime.area) || switcher.open)) {
      return;
    }
    if (ime.area && event.target === ime.area && (event.isComposing || event.keyCode === 229)) {
      return;	// (the host's input method has this key: what it composes comes as text, imeFrame)
    }
    const code = KEY_CODES[event.code];
    if (code === undefined) {
      return;
    }
    event.stopImmediatePropagation();	// (not the runtime's: this is the window server's now)
    if (event.repeat) {
      event.preventDefault();	// (the clients repeat keys themselves)
      return;
    }
    if (clipboard.pendingHostWrite !== null) {
      writeHost(clipboard.pendingHostWrite);
    }
    phys.add(event.code);
    const alt = phys.has("AltLeft") || phys.has("AltRight");
    const ctrl = phys.has("ControlLeft") || phys.has("ControlRight");
    if (meta && event.code !== meta.code) {
      meta.used = true;
    }
    // Escape closes the launcher, wherever the keyboard is.
    if (event.code === "Escape" && launcher && launcher.style.display !== "none") {
      event.preventDefault();
      swallowed.add(event.code);
      toggleLauncher(false);
      return;
    }
    // The switcher's keys while it is up.
    if (switcher.open) {
      event.preventDefault();
      swallowed.add(event.code);
      if (event.code === "Tab" || event.code === "Backquote") {
        cycleSwitcher(event.shiftKey ? -1 : 1);
      } else if (event.code === "ArrowRight" || event.code === "ArrowDown") {
        cycleSwitcher(1);
      } else if (event.code === "ArrowLeft" || event.code === "ArrowUp") {
        cycleSwitcher(-1);
      } else if (event.code === "Escape") {
        closeSwitcher(false);
      } else if (event.code === "Enter") {
        closeSwitcher(true);
      }
      return;
    }
    // Window keys.
    if (alt && (event.code === "Tab" || event.code === "Backquote")) {
      event.preventDefault();
      swallowed.add(event.code);
      // The guest is told the modifiers went up: the keyboard is about to go to another window.
      for (const c of [29, 97, 42, 54, 56, 100, 125, 126]) {
        emit(c, false);
      }
      openSwitcher(event.shiftKey ? -1 : 1, ctrl ? "ctrl-alt-tab" : event.code === "Tab" ? "alt-tab" : "alt-`");
      return;
    }
    if (alt && (phys.has("ShiftLeft") || phys.has("ShiftRight")) && event.code === "KeyO") {
      event.preventDefault();
      swallowed.add(event.code);
      sendPage();
      return;
    }
    // Virtual desktops (KWin 6's keys; PLASMA-PROTOCOL.md section 1), when the server has them.
    const shift = phys.has("ShiftLeft") || phys.has("ShiftRight");
    const metaHeld = !!meta || phys.has("MetaLeft") || phys.has("MetaRight");
    const arrows = { ArrowLeft: 1, ArrowRight: 2, ArrowUp: 3, ArrowDown: 4 };
    if (shellVersion && ctrl && !alt && !metaHeld && /^F[1-9]$/.test(event.code)) {
      event.preventDefault();
      swallowed.add(event.code);
      order(ORDER_SET_DESKTOP, 0, Number(event.code.slice(1)));
      return;
    }
    if (shellVersion && ctrl && ((metaHeld && arrows[event.code]) || (alt && (event.code === "ArrowLeft" || event.code === "ArrowRight")))) {
      event.preventDefault();
      swallowed.add(event.code);
      if (meta) {
        meta.used = true;
      }
      order(ORDER_DESKTOP_STEP, 0, arrows[event.code], metaHeld && shift ? 1 : 0);
      return;
    }
    if (shellVersion && metaHeld && !ctrl && !alt && event.code === "KeyD") {
      event.preventDefault();
      swallowed.add(event.code);
      if (meta) {
        meta.used = true;
      }
      order(ORDER_SHOW_DESKTOP, 0, 2);
      return;
    }
    if (metaHeld && !ctrl && !alt && event.code === "KeyW" && theme()) {
      event.preventDefault();
      swallowed.add(event.code);
      if (meta) {
        meta.used = true;
      }
      toggleOverview();
      return;
    }
    // KWin's default keys for the active window (kwin's src/useractions.cpp): Meta+Left, Right, Up and Down
    // tile it to that half of the screen (the same key again gives it its size back), Meta+PgUp maximizes or
    // restores it, Meta+PgDown minimizes it.
    const TILE_KEYS = { ArrowLeft: 1, ArrowRight: 2, ArrowUp: 4, ArrowDown: 8 };
    if (metaHeld && !ctrl && !alt && !shift && (TILE_KEYS[event.code] || event.code === "PageUp" || event.code === "PageDown")) {
      event.preventDefault();
      swallowed.add(event.code);
      if (meta) {
        meta.used = true;
      }
      const focus = tableNow.v.getUint32(tableNow.t + H_FOCUS, true);
      const w = focus && windowList().find((x) => x.id === focus);
      if (!w) {
        return;
      }
      if (event.code === "PageUp") {
        order(ORDER_MAXIMIZE, focus);
      } else if (event.code === "PageDown") {
        order(ORDER_MINIMIZE, focus);
      } else if (shellVersion) {
        order(ORDER_TILE, focus, w.tile === TILE_KEYS[event.code] ? 0 : TILE_KEYS[event.code]);
      } else {
        quickTile(focus, TILE_KEYS[event.code]);
      }
      return;
    }
    // Plasma's default keys for programs (their .desktop files' X-KDE-Shortcuts): Ctrl+Alt+T Konsole, Meta+E
    // Dolphin; Alt+F2 and Alt+Space (KRunner's) open the launcher to search.
    const program = ctrl && alt && !metaHeld && event.code === "KeyT" ? "org.kde.konsole.desktop"
      : metaHeld && !ctrl && !alt && event.code === "KeyE" ? "org.kde.dolphin.desktop" : null;
    if (program || (alt && !ctrl && !metaHeld && (event.code === "F2" || event.code === "Space"))) {
      event.preventDefault();
      swallowed.add(event.code);
      if (meta) {
        meta.used = true;
      }
      if (!program) {
        toggleLauncher(true);
        return;
      }
      const index = apps.findIndex((a) => a.file === program);
      if (index >= 0) {
        launch(index);
      } else {
        note("no " + program + " here");
      }
      return;
    }
    if (alt && event.code === "F4") {
      event.preventDefault();
      swallowed.add(event.code);
      const focus = tableNow.v.getUint32(tableNow.t + H_FOCUS, true);
      if (focus) {
        order(ORDER_CLOSE, focus);
      }
      return;
    }
    // Cmd (or Super): held back until it is known what it is for.
    if (event.code === "MetaLeft" || event.code === "MetaRight") {
      meta = { code: event.code, sent: false, used: false };
      if (settings.cmdAsCtrl) {
        event.preventDefault();
        return;
      }
      meta.sent = true;
      forward(event, code);
      return;
    }
    if (meta && settings.cmdAsCtrl) {
      if (CMD_KEYS.has(event.code)) {
        const shift = TERMINAL_KEYS.has(event.code) && TERMINALS.test(focusedApp());
        if (meta.sent) {
          emit(KEY_CODES[meta.code], false);
          meta.sent = false;
        }
        emit(CTRL, true);
        if (shift) {
          emit(SHIFT, true);
        }
        remapped.set(event.code, { shift });
      } else if (!meta.sent) {
        emit(KEY_CODES[meta.code], true);
        meta.sent = true;
      }
    }
    forward(event, code);
  };

  const onKeyUp = (event) => {
    if (!active || !os) {
      return;
    }
    const code = KEY_CODES[event.code];
    if (code === undefined || !(phys.has(event.code) || guestDown.has(code))) {
      return;
    }
    event.stopImmediatePropagation();
    event.preventDefault();
    phys.delete(event.code);
    if (switcher.open && !phys.has("AltLeft") && !phys.has("AltRight")) {
      closeSwitcher(true);	// (Alt let go: the window picked comes forward)
    }
    if (swallowed.delete(event.code)) {
      return;
    }
    if (meta && event.code === meta.code) {
      const m = meta;
      meta = null;
      if (m.sent) {
        emit(code, false);
      }
      if (!m.used) {
        toggleLauncher();	// (Super, or Cmd, alone: the panel's launcher)
      }
      return;
    }
    emit(code, false);
    const r = remapped.get(event.code);
    if (r) {
      remapped.delete(event.code);
      if (r.shift) {
        emit(SHIFT, false);
      }
      emit(CTRL, false);
    }
  };

  // ---- the page of the browser window in front, to the host's browser (Alt-Shift-O, the bar's "Page to host")
  //
  // For a heavy page someone opened in the machine's Firefox or Chromium (video above all): its address is
  // read the way a person would, Ctrl-L (the address bar) and Ctrl-C, which puts it on the guest's clipboard,
  // and so, by the bridge, here (the clipboard above); then Escape closes the address bar's list. The address
  // goes to hostopen.js, which opens it at once if the key or the click that asked is still the page's user
  // activation (Chrome: five seconds), and shows it as a link to click otherwise. What it costs: the address
  // is on the guest's and the host's clipboard afterwards, and the address bar keeps the keyboard.
  const BROWSERS = /firefox|chromium|chrome|falkon|epiphany|konqueror|midori/i;
  let sending = false, lastSend = null;
  const sendPage = async () => {
    lastSend = await sendPageNow();
    say("display: page to host: " + JSON.stringify(lastSend));
    return lastSend;
  };
  const sendPageNow = async () => {
    if (!active || !tableNow || sending) {
      return { status: "busy" };
    }
    const app = focusedApp();
    if (!BROWSERS.test(app)) {
      note("Page to host: the window in front is not a web browser" + (app.trim() ? " (" + app.trim() + ")" : ""));
      return { status: "not a browser", app };
    }
    if (!window.rvHostOpen) {
      note("Page to host: no hostopen.js on this page");
      return { status: "no hostopen" };
    }
    sending = true;
    try {
      // The modifiers up for the guest (Alt and Shift of this very key are down), then the keys.
      for (const c of [29, 97, 42, 54, 56, 100, 125, 126]) {
        emit(c, false);
      }
      const before = clipboard.fromGuest;
      for (const [c, k] of [[29, 38], [29, 46]]) {	// Ctrl-L, Ctrl-C
        send(c, true);
        send(k, true);
        send(k, false);
        send(c, false);
      }
      const began = performance.now();
      // (Up to 10 s: a busy Firefox under the emulator can take that long; after Chrome's five seconds of user
      // activation the address is shown as a link instead of opened.)
      while (clipboard.fromGuest === before && performance.now() - began < 10000) {
        await new Promise((resolve) => setTimeout(resolve, 30));
      }
      send(1, true);	// Escape: the address bar's list closes
      send(1, false);
      if (clipboard.fromGuest === before) {
        note("Page to host: the browser did not copy its address (no answer in 10 s): try again");
        return { status: "no address" };
      }
      const url = (clipboard.lastGuestText || "").trim();
      const result = window.rvHostOpen.ask(url, { direct: true });
      note(result.status === "opened" ? "Page to host: opened " + url : result.status === "asked"
        ? "Page to host: click the address in the top right corner" : "Page to host: " + result.reason);
      return Object.assign({ url }, result);
    } finally {
      sending = false;
    }
  };

  // A paste: V waits (with every key after it) until the server has the host's text as the selection.
  const startPaste = () => {
    queue = [[KEY_V, true]];
    let got = false;
    const finish = (value) => {
      if (got) {
        return;
      }
      got = true;
      document.removeEventListener("paste", onPaste, true);
      giveHost(value).then(() => {
        const held = queue || [];
        queue = null;
        for (const [c, d] of held) {
          send(c, d);
        }
      });
    };
    const onPaste = (e) => {
      e.preventDefault();
      finish(e.clipboardData ? e.clipboardData.getData("text/plain") : null);
    };
    document.addEventListener("paste", onPaste, true);
    setTimeout(() => {
      if (got) {
        return;
      }
      // No paste event (Ctrl-V on a Mac, a key sent by a test driver): ask for the text.
      if (navigator.clipboard && navigator.clipboard.readText) {
        const timer = setTimeout(() => finish(null), 3000);
        navigator.clipboard.readText().then((value) => {
          clearTimeout(timer);
          finish(value);
        }, (error) => {
          clearTimeout(timer);
          clipboard.lastError = String(error && error.message || error);
          finish(null);
        });
      } else {
        finish(null);
      }
    }, 50);
  };

  // The page lost the keyboard (another window, another tab): nothing stays down in the guest.
  const lostKeyboard = () => {
    phys.clear();
    meta = null;
    remapped.clear();
    swallowed.clear();
    if (switcher.open) {
      closeSwitcher(false);
    }
    releaseAll();
  };

  // ---- text from the host's input method (Japanese, Chinese, Korean, ...)
  //
  // Keys go to the guest as keys, and its programs make characters of them with the keymap; text that no key
  // makes comes from an input method, which composes it from several keys and a choice. The host has one (the
  // Mac's Japanese input, say), but it works only in a text field: so while the window with the keyboard has
  // a text input enabled (the server says so, wlserver/textinput.js: a Qt or GTK text field has the focus),
  // the keyboard goes to a field of the page's own at that window's text cursor instead of the canvas. Keys
  // the input method does not take (event.keyCode 229) still go to the guest as keys, as before; what it
  // composes is shown in the field, and what is committed goes to the server in pieces of at most TEXT_CAP
  // bytes (the table's text words), which hands it to the window as text_input's commit_string.
  const ime = { area: null, state: 0, composing: false, queue: [], sent: 0, at: "" };
  const imeSetup = () => {
    const a = document.createElement("textarea");
    a.className = "rv-ime";
    a.setAttribute("autocomplete", "off");
    a.setAttribute("autocorrect", "off");
    a.setAttribute("autocapitalize", "off");
    a.spellcheck = false;
    a.tabIndex = -1;
    a.style.cssText = "position: absolute; left: 0; top: 0; width: 1px; height: 1px; padding: 0 2px; margin: 0; border: 0;"
      + " resize: none; overflow: hidden; white-space: pre; opacity: 0; z-index: 30; outline: none; color: #000;"
      + " background: #fffbe6; box-shadow: 0 1px 4px rgba(0,0,0,.35); font: 16px sans-serif; pointer-events: none;";
    a.addEventListener("compositionstart", () => {
      ime.composing = true;
      a.style.opacity = "1";
      a.style.width = "auto";
    });
    a.addEventListener("compositionupdate", (event) => {
      a.style.width = Math.max(8, (event.data || "").length + 1) + "em";
    });
    a.addEventListener("compositionend", (event) => {
      ime.composing = false;
      a.style.opacity = "0";
      a.style.width = "1px";
      imeCommit(event.data || "");
      a.value = "";
    });
    a.addEventListener("input", (event) => {
      if (!ime.composing && event.inputType === "insertText" && event.data) {
        imeCommit(event.data);	// (an input method that inserts without composing: full-width punctuation)
      }
      if (!ime.composing) {
        a.value = "";
      }
    });
    a.addEventListener("blur", () => {
      if (!ime.composing) {
        a.value = "";
      }
    });
    const stage = screen() && screen().parentElement;
    (stage || document.body).appendChild(a);
    ime.area = a;
  };
  const imeCommit = (text) => {
    if (!text) {
      return;
    }
    // Pieces of at most TEXT_CAP bytes, cut between characters.
    let piece = "";
    for (const ch of text) {
      if (encoder.encode(piece + ch).length > TEXT_CAP) {
        ime.queue.push(piece);
        piece = "";
      }
      piece += ch;
    }
    if (piece) {
      ime.queue.push(piece);
    }
  };
  const imeFrame = (v, w32, buffer, t) => {
    if (!ime.area) {
      return;
    }
    // The next piece, once the server has taken the one before.
    if (ime.queue.length && (v.getUint32(t + H_TEXT_ACK, true) >>> 0) === (v.getUint32(t + H_TEXT_SERIAL, true) >>> 0)) {
      const raw = encoder.encode(ime.queue.shift());
      new Uint8Array(buffer).set(raw, t + H_TEXT);
      Atomics.store(w32, (t + H_TEXT_LEN) / 4, raw.length);
      Atomics.store(w32, (t + H_TEXT_SERIAL) / 4, (v.getUint32(t + H_TEXT_SERIAL, true) + 1) | 0);
      ringBell();
    }
    const state = v.getUint32(t + H_IME_STATE, true);
    if (state) {
      const sc = screen(), box = sc.getBoundingClientRect(), stageBox = sc.parentElement.getBoundingClientRect();
      const k = box.width / screenNow[0];
      const x = v.getInt32(t + H_IME_X, true), y = v.getInt32(t + H_IME_Y, true), h = v.getInt32(t + H_IME_H, true);
      const at = x + "," + y + "," + h + "," + k;
      if (at !== ime.at) {
        ime.at = at;
        const a = ime.area;
        a.style.left = (box.left - stageBox.left + x * k) + "px";
        a.style.top = (box.top - stageBox.top + y * k) + "px";
        a.style.fontSize = Math.max(10, Math.round(h * k * 0.8)) + "px";
        a.style.height = Math.max(12, Math.round(h * k)) + "px";
        a.style.lineHeight = a.style.height;
      }
    }
    if (state !== ime.state) {
      ime.state = state;
      if (state && document.activeElement === screen()) {
        ime.area.focus({ preventScroll: true });
      } else if (!state && document.activeElement === ime.area && !ime.composing) {
        focusScreen();
      }
    } else if (state && document.activeElement === screen()) {
      ime.area.focus({ preventScroll: true });	// (a click on the canvas gave the screen the keyboard back)
    }
  };

  // ---- the layout

  // A guess from the host's layout (Chrome tells what each key types, unmodified), or null.
  const guessLayout = async () => {
    if (!navigator.keyboard || !navigator.keyboard.getLayoutMap) {
      return null;
    }
    try {
      const m = await navigator.keyboard.getLayoutMap();
      const k = (code) => m.get(code) || "";
      if (k("KeyQ") === "й") return k("Backquote") === "ё" || k("KeyS") === "ы" ? "ru" : "ua";
      if (k("KeyQ") === ";" || k("KeyW") === "ς") return "gr";
      if (k("KeyQ") === "'" && k("KeyS") === "o") return "us-dvorak";
      if (k("KeyS") === "r" && k("KeyE") === "f") return "us-colemak";
      if (k("KeyQ") === "a") return k("Semicolon") === "m" && k("BracketLeft") === "^" && k("Digit1") === "&" ? "fr" : "be";
      if (k("KeyY") === "z") {
        if (k("Semicolon") === "é" && k("Quote") === "à") return "ch-fr";
        if (k("Semicolon") === "ö" && k("Quote") === "ä" && k("BracketLeft") === "ü" && k("Minus") === "ß") return "de";
        if (k("BracketLeft") === "ü") return "ch";
        if (k("Semicolon") === "ů") return "cz";
        if (k("Digit0") === "ö") return "hu";
        return "de";
      }
      if (k("Semicolon") === "ö" && k("Quote") === "ä") return k("BracketLeft") === "å" ? "se" : "fi";
      if (k("Semicolon") === "ø") return "no";
      if (k("Semicolon") === "æ") return "dk";
      if (k("Semicolon") === "ñ") return k("Quote") === "{" || k("BracketLeft") === "´" ? "latam" : "es";
      if (k("Semicolon") === "ò") return "it";
      if (k("Semicolon") === "ç") return k("Quote") === "~" ? "br" : "pt";
      if (k("Semicolon") === "ş") return "tr";
      if (k("BracketLeft") === "@" && k("Quote") === ":") return "jp";
      if (k("Semicolon") === "+" && k("Quote") === "´") return "nl";
      if (k("Backslash") === "#") return "gb";
      return "us";
    } catch (error) {
      return null;
    }
  };

  let layoutWanted = params.get("kbd") || settings.layout || null;
  let layoutGuessed = null;
  guessLayout().then((name) => {
    layoutGuessed = name;
    if (!layoutWanted && name) {
      sendLayout();
    }
  });
  const layoutName = () => (layoutWanted || layoutGuessed || "us").replace(/[()]/g, "-").replace(/-$/, "");

  // Tell the server which layout (when its table appears, and when the person picks another).
  const sendLayout = () => {
    if (!active || !tableNow || !os) {
      return;
    }
    const { t } = tableNow;
    const buffer = os.memory.buffer;
    const v = new DataView(buffer), w32 = new Int32Array(buffer);
    const m = new Uint8Array(buffer);
    m.fill(0, t + H_KBD_NAME, t + H_KBD_NAME + 32);
    m.set(encoder.encode(layoutName()).subarray(0, 31), t + H_KBD_NAME);
    Atomics.store(w32, (t + H_KBD_SERIAL) / 4, (v.getUint32(t + H_KBD_SERIAL, true) + 1) | 0);
    ringBell();
  };
  const setLayout = (name) => {
    layoutWanted = name;
    settings.layout = name;
    saveSettings();
    sendLayout();
  };

  // ---- windows by recency: the switcher (Alt-Tab) and the list (the bar's button, Super alone)

  const mru = [];		// window ids, the one that had the keyboard last first
  let lastFocus = 0;
  const trackFocus = (focus) => {
    if (focus && focus !== lastFocus) {
      const at = mru.indexOf(focus);
      if (at >= 0) {
        mru.splice(at, 1);
      }
      mru.unshift(focus);
    }
    lastFocus = focus;
  };

  // The windows (toplevels, minimized ones too), the most recent first: [{ id, title, app, minimized }].
  const windowList = () => {
    const buffer = os.memory.buffer;
    const tops = lastWindows.filter((s) => s.kind === KIND_TOPLEVEL);
    const rank = (s) => {
      const at = mru.indexOf(s.id);
      return at >= 0 ? at : 1000 - s.z;
    };
    const v = new DataView(buffer);
    const shell = shellVersion;
    return tops.sort((a, b) => rank(a) - rank(b)).map((s) => ({
      id: s.id, title: text(buffer, s.at + S_TITLE), app: text(buffer, s.at + S_APP_ID),
      minimized: !!(s.flags & F_MINIMIZED), maximized: !!(s.flags & F_MAXIMIZED), flags: s.flags,
      // (PLASMA-PROTOCOL.md: the desktop, 0 all; shell flags; quick tile)
      desktop: shell ? v.getUint32(s.at + S_DESKTOP, true) : 1, shellFlags: shell ? v.getUint32(s.at + S_SHELL_FLAGS, true) : 0,
      tile: shell ? v.getUint32(s.at + S_TILE, true) : 0,
      box: [(s.sx === undefined ? s.x : s.sx) + s.geometry[0], (s.sy === undefined ? s.y : s.sy) + s.geometry[1], s.geometry[2], s.geometry[3]],
    }));
  };

  const bring = (w) => order(w.minimized ? ORDER_RESTORE : ORDER_ACTIVATE, w.id);

  const switcher = { open: false, el: null, items: [], at: 0, how: "" };
  const renderSwitcher = () => {
    const el = switcher.el;
    const T = theme();
    if (T) {
      T.renderSwitcher(el, switcher.items, switcher.at, switcher.thumbs || new Map());
      return;
    }
    el.textContent = "";
    const head = document.createElement("div");
    head.className = "rv-switcher-head";
    head.textContent = "Windows";
    el.appendChild(head);
    switcher.items.forEach((w, i) => {
      const row = document.createElement("div");
      row.className = "rv-switcher-item" + (i === switcher.at ? " rv-selected" : "") + (w.minimized ? " rv-minimized" : "");
      row.textContent = (w.title || w.app || "window " + w.id) + (w.minimized ? "  (minimized)" : "");
      el.appendChild(row);
    });
  };
  const openSwitcher = (direction, how) => {
    const items = windowList().filter((w) => !(w.shellFlags & 16));	// (skip switcher)
    if (!items.length) {
      return;
    }
    switcher.items = items;
    switcher.how = how;
    switcher.open = true;
    switcher.at = items.length > 1 ? (direction > 0 ? 1 : items.length - 1) : 0;
    if (!switcher.el) {
      switcher.el = document.createElement("div");
      switcher.el.id = "rv-switcher";
      stage.appendChild(switcher.el);
    }
    switcher.el.style.display = "";
    // (Plasma's switcher shows the windows' pictures: taken once, as they are when it comes up)
    switcher.thumbs = theme() ? new Map(items.map((w) => [w.id, thumbnail(w.id, 288, 180)])) : null;
    renderSwitcher();
    stats.switcher = { opened: (stats.switcher ? stats.switcher.opened : 0) + 1, how };
  };
  // KWin's Overview (Meta+W): plasma.js draws it; the windows of this desktop, their pictures taken now.
  let overview = null;
  const toggleOverview = () => {
    const T = theme();
    if (overview || !T) {
      if (overview) {
        overview.remove();
        overview = null;
        focusScreen();
      }
      return;
    }
    const items = windowList().filter((w) => !w.minimized && !(w.shellFlags & 16) && (w.desktop === 0 || w.desktop === desktopNow));
    overview = document.createElement("div");
    stage.appendChild(overview);
    const close = () => {
      if (overview) {
        overview.remove();
        overview = null;
        focusScreen();
      }
    };
    T.renderOverview(overview, items, new Map(items.map((w) => [w.id, thumbnail(w.id, 960, 640)])), (w) => {
      close();
      bring(w);
    }, close);
  };

  const cycleSwitcher = (direction) => {
    const n = switcher.items.length;
    switcher.at = ((switcher.at + direction) % n + n) % n;
    renderSwitcher();
  };
  const closeSwitcher = (pick) => {
    switcher.open = false;
    if (switcher.el) {
      switcher.el.style.display = "none";
    }
    const w = switcher.items[switcher.at];
    if (pick && w) {
      bring(w);
      stats.switcher.picked = w.title || w.app || String(w.id);
    }
  };

  // ---- the panel: the strip at the bottom of the screen (panel.css)
  //
  // A launcher (the programs the server found in the guest's .desktop files; picking one is an order, which
  // the server carries out by starting it in the session), the windows (minimized ones too: a click brings
  // one forward, or minimizes the one that is in front), and a clock. The server keeps windows off the strip
  // (the screen request says how high it is); a fullscreen window in front covers it.

  let panel = null, tasks = null, clock = null, launcher = null, launcherList = null, launcherFilter = null;
  let apps = [], appsSerial = 0, tasksShown = "";

  const setupPanel = () => {
    const link = document.createElement("link");
    link.rel = "stylesheet";
    link.href = "panel.css";
    document.head.appendChild(link);
    panel = document.createElement("div");
    panel.id = "rv-panel";
    panel.style.display = "none";
    panel.style.height = PANEL_HEIGHT + "px";
    // (No new panel while a button is held on it: the element pressed would be gone before the click.)
    panel.addEventListener("pointerdown", () => {
      panelHeld = true;
    }, true);
    for (const type of ["pointerup", "pointercancel"]) {
      window.addEventListener(type, () => {
        panelHeld = false;
      }, true);
    }
    const start = document.createElement("button");
    start.className = "rv-panel-start";
    start.textContent = "Apps";
    start.title = "The programs (also: Super, or Cmd, pressed alone)";
    start.tabIndex = -1;
    start.addEventListener("pointerdown", (event) => event.preventDefault());
    start.addEventListener("click", () => toggleLauncher());
    panel.appendChild(start);
    tasks = document.createElement("div");
    tasks.className = "rv-panel-tasks";
    panel.appendChild(tasks);
    clock = document.createElement("div");
    clock.className = "rv-panel-clock";
    panel.appendChild(clock);
    stage.appendChild(panel);
    launcher = document.createElement("div");
    launcher.id = "rv-launcher";
    launcher.style.display = "none";
    launcherFilter = document.createElement("input");
    launcherFilter.className = "rv-launcher-filter";
    launcherFilter.placeholder = "Type to find a program";
    launcherFilter.addEventListener("input", renderLauncher);
    launcherFilter.addEventListener("keydown", (event) => {
      event.stopPropagation();
      if (event.key === "Escape") {
        toggleLauncher(false);
      } else if (event.key === "Enter") {
        const first = launcherList.querySelector(".rv-launcher-item");
        if (first) {
          first.click();
        }
      }
    });
    launcher.appendChild(launcherFilter);
    launcherList = document.createElement("div");
    launcherList.className = "rv-launcher-list";
    launcher.appendChild(launcherList);
    stage.appendChild(launcher);
    // A click anywhere else closes the launcher.
    window.addEventListener("pointerdown", (event) => {
      if (launcher.style.display !== "none" && !launcher.contains(event.target) && !event.target.closest(".rv-panel-start")) {
        toggleLauncher(false);
      }
    }, true);
    const tick = () => {
      const d = new Date();
      clock.textContent = d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
      clock.title = d.toLocaleDateString([], { weekday: "long", year: "numeric", month: "long", day: "numeric" });
    };
    tick();
    setInterval(tick, 1000);
  };

  const launch = (index) => {
    order(ORDER_LAUNCH, 0, index, appsSerial | 0);
    note("starting " + apps[index].name + "...");
    toggleLauncher(false);
  };

  const renderLauncher = () => {
    const want = launcherFilter.value.trim().toLowerCase();
    launcherList.textContent = "";
    apps.forEach((app, i) => {
      if (want && !(app.name + " " + app.comment + " " + app.categories).toLowerCase().includes(want)) {
        return;
      }
      const b = document.createElement("button");
      b.className = "rv-launcher-item";
      b.tabIndex = -1;
      const name = document.createElement("span");
      name.className = "rv-launcher-name";
      name.textContent = app.name;
      b.appendChild(name);
      if (app.comment) {
        const c = document.createElement("span");
        c.className = "rv-launcher-comment";
        c.textContent = app.comment;
        b.appendChild(c);
      }
      b.addEventListener("click", () => launch(i));
      launcherList.appendChild(b);
    });
    if (!launcherList.firstChild) {
      const none = document.createElement("div");
      none.className = "rv-launcher-none";
      none.textContent = apps.length ? "Nothing matches" : "No programs found";
      launcherList.appendChild(none);
    }
  };

  const toggleLauncher = (open) => {
    if (!launcher || !active) {
      return;
    }
    const show = open === undefined ? launcher.style.display === "none" : open;
    if (show && nativeShell) {
      return;	// (plasmashell's Kickoff is the launcher; the page's lives in its panel, which is not shown)
    }
    const T = theme();
    if (T) {
      launcher.style.display = show ? "" : "none";
      if (show) {
        T.openLauncher(launcher, panelModel());
      } else {
        focusScreen();
      }
      return;
    }
    launcher.style.display = show ? "" : "none";
    if (show) {
      launcherFilter.value = "";
      renderLauncher();
      launcherFilter.focus({ preventScroll: true });
    } else {
      focusScreen();
    }
  };

  // What plasma.js draws the panel and the launcher from.
  // The Plasma shell's state from the table (PLASMA-PROTOCOL.md), when the server has it: { version, count, current,
  // rows, names }.
  let desktopsSerial = -1, desktopNames = [];
  const shellState = () => {
    if (!tableNow || !os) {
      return { version: 0, count: 1, current: 1, rows: 1, names: [] };
    }
    const { v, t } = tableNow;
    const version = v.getUint32(t + H_SHELL_VERSION, true);
    if (!version) {
      return { version: 0, count: 1, current: 1, rows: 1, names: [] };
    }
    const serial = v.getUint32(t + H_DESKTOPS_SERIAL, true);
    if (serial !== desktopsSerial) {
      try {
        const at = v.getUint32(t + H_DESKTOPS_LO, true) + v.getUint32(t + H_DESKTOPS_HI, true) * 0x100000000;
        const j = JSON.parse(decoder.decode(new Uint8Array(os.memory.buffer, at, v.getUint32(t + H_DESKTOPS_LEN, true)).slice()));
        desktopNames = Array.isArray(j) ? j : (j && j.desktops) || [];
        desktopsSerial = serial;
      } catch (error) {
        desktopNames = [];
      }
    }
    return { version, count: Math.max(1, v.getUint32(t + H_DESKTOPS, true)), current: Math.max(1, v.getUint32(t + H_DESKTOP_CURRENT, true)),
             showing: v.getUint32(t + H_SHOWING_DESKTOP, true) === 1,
             rows: Math.max(1, v.getUint32(t + H_DESKTOP_ROWS, true)), names: desktopNames };
  };

  const panelModel = () => {
    const anyMaximized = lastWindows.some((s) => s.kind === KIND_TOPLEVEL && (s.flags & F_MAXIMIZED) && !(s.flags & F_MINIMIZED));
    return {
      windows: windowList(), focus: lastFocus, apps, anyMaximized, screen: screenNow.slice(),
      layout: tableNow && os ? text(os.memory.buffer, tableNow.t + H_KBD_CURRENT).slice(0, 32) : "",
      launch: (i) => launch(i), bring: (w) => bring(w), minimize: (id) => order(ORDER_MINIMIZE, id),
      shell: shellState(), order: (op, id, a = 0, b = 0, c = 0, d = 0) => order(op, id, a, b, c, d),
      toggleLauncher: (show) => toggleLauncher(show), focusScreen,
      // Plasma's "Peek at Desktop": every window minimized, or the ones it minimized back.
      showDesktop: () => {
        if (shellVersion) {
          order(ORDER_SHOW_DESKTOP, 0, 2);	// (the server's "show desktop", toggled)
          return;
        }
        const list = windowList();
        if (shownDesktop && list.every((w) => w.minimized)) {
          for (const id of shownDesktop) {
            order(ORDER_RESTORE, id);
          }
          shownDesktop = null;
        } else {
          shownDesktop = list.filter((w) => !w.minimized).map((w) => w.id);
          for (const id of shownDesktop) {
            order(ORDER_MINIMIZE, id);
          }
        }
      },
    };
  };
  let shownDesktop = null, panelShown = "", panelHeld = false;

  const panelFrame = (v, t) => {
    if (!panel) {
      return;
    }
    if (nativeShell) {
      if (panel.style.display !== "none") {
        panel.style.display = "none";	// (plasmashell's own panels are in the table)
        panelShown = "";
      }
      return;
    }
    const T = theme();
    if (T) {
      // Plasma's panel (plasma.js), over the strip the server keeps: laid out in the screen's pixels, scaled as the
      // frames are.
      const serial = table.version >= 3 ? v.getUint32(t + H_APPS_SERIAL, true) : 0;
      if (serial !== appsSerial) {
        appsSerial = serial;
        try {
          const at = v.getUint32(t + H_APPS_LO, true) + v.getUint32(t + H_APPS_HI, true) * 0x100000000;
          apps = JSON.parse(decoder.decode(new Uint8Array(os.memory.buffer, at, v.getUint32(t + H_APPS_LEN, true)).slice()));
        } catch (error) {
          apps = [];
          appsSerial = 0;
        }
      }
      const focus = v.getUint32(t + H_FOCUS, true);
      const front = lastWindows.find((s) => s.id === focus);
      const display = front && (front.flags & F_FULLSCREEN) ? "none" : "";
      if (panel.style.display !== display) {
        panel.style.display = display;
      }
      const box = screen().getBoundingClientRect();
      const k = box.width / screenNow[0];
      const geometry = [screenNow[0], screenNow[1], k.toFixed(4)].join(",");
      const model = panelModel();
      const sig = geometry + "|" + T.serial + "|" + appsSerial + "|" + model.anyMaximized + "|" + model.layout + "|"
        + model.shell.version + "," + model.shell.count + "," + model.shell.current + "," + desktopsSerial + "," + model.shell.showing + "|"
        + model.windows.map((w) => w.id + ":" + w.minimized + ":" + w.title + ":" + w.app + ":" + w.desktop + ":" + w.shellFlags
          + (w.box ? ":" + w.box.map((v) => Math.round(v / 24)).join(",") : "")).join("|") + "|" + focus;	// (the pager draws where they are, roughly)
      if (sig === panelShown || panelHeld) {
        return;
      }
      panelShown = sig;
      // (The panels' layer is the whole screen, scaled as the frames are; the launcher and the panels' popups are
      // in it too, placed by plasma.js.)
      const st = panel.style;
      st.top = "0px";
      st.left = "0px";
      st.width = screenNow[0] + "px";
      st.height = screenNow[1] + "px";
      st.transform = "scale(" + k + ")";
      st.transformOrigin = "0 0";
      if (launcher.parentNode !== panel) {
        panel.appendChild(launcher);
        launcher.style.top = "";
        launcher.style.transform = "";
      }
      T.renderPanel(panel, model);
      return;
    }
    // The programs, when the server has a new list.
    const serial = table.version >= 3 ? v.getUint32(t + H_APPS_SERIAL, true) : 0;
    if (serial !== appsSerial) {
      appsSerial = serial;
      try {
        const at = v.getUint32(t + H_APPS_LO, true) + v.getUint32(t + H_APPS_HI, true) * 0x100000000;
        const len = v.getUint32(t + H_APPS_LEN, true);
        apps = JSON.parse(decoder.decode(new Uint8Array(os.memory.buffer, at, len).slice()));
      } catch (error) {
        apps = [];
        appsSerial = 0;	// (read while it was being replaced: again next frame)
      }
    }
    // Hidden under a fullscreen window in front.
    const focus = v.getUint32(t + H_FOCUS, true);
    const front = lastWindows.find((s) => s.id === focus);
    const covered = !!(front && (front.flags & F_FULLSCREEN));
    const display = covered ? "none" : "";
    if (panel.style.display !== display) {
      panel.style.display = display;
    }
    // At the bottom of the screen (placed by the screen's box: #screen's margin is not the stage's business).
    const box = screen().getBoundingClientRect();
    const k = box.width / screenNow[0];
    const top = Math.round(box.height - PANEL_HEIGHT * k) + "px", width = Math.round(box.width) + "px";
    if (panel.style.top !== top || panel.style.width !== width) {
      panel.style.top = top;
      panel.style.width = width;
      panel.style.height = Math.round(PANEL_HEIGHT * k) + "px";
      launcher.style.top = top;
    }
    // The windows.
    const list = windowList();
    const sig = list.map((w) => w.id + ":" + w.minimized + ":" + w.title).join("|") + "|" + focus;
    if (sig === tasksShown) {
      return;
    }
    tasksShown = sig;
    tasks.textContent = "";
    // (In the order they came, as a taskbar keeps them, not by recency.)
    for (const w of list.slice().sort((a, b) => a.id - b.id)) {
      const b = document.createElement("button");
      b.className = "rv-task" + (w.id === focus && !w.minimized ? " rv-active" : "") + (w.minimized ? " rv-minimized" : "");
      b.tabIndex = -1;
      b.textContent = w.title || w.app || "window " + w.id;
      b.title = b.textContent + (w.minimized ? " (minimized)" : "");
      b.addEventListener("pointerdown", (event) => {
        event.preventDefault();
        focusScreen();
      });
      b.addEventListener("click", () => {
        if (w.id === lastFocus && !w.minimized) {
          order(ORDER_MINIMIZE, w.id);
        } else {
          bring(w);
        }
      });
      tasks.appendChild(b);
    }
  };

  // ---- the bar: fullscreen, the layout, Cmd

  const fullscreen = async () => {
    if (document.fullscreenElement === stage) {
      await document.exitFullscreen().catch(() => {});
      return;
    }
    try {
      await stage.requestFullscreen({ navigationUI: "hide" });
      // In fullscreen Chrome lets a page have the keys the system would take (Alt-Tab, Alt-F4, Cmd-W, ...);
      // Escape held down leaves.
      if (navigator.keyboard && navigator.keyboard.lock) {
        await navigator.keyboard.lock().catch((error) => say("display: no keyboard lock: " + error.message));
      }
    } catch (error) {
      say("display: no fullscreen: " + error.message);
    }
    focusScreen();
  };

  let layoutSelect = null, layoutsShown = "", barStatus = null;
  const setupBar = () => {
    const button = (label, what, run) => {
      const b = document.createElement("button");
      b.className = "rv-bar-btn";
      b.textContent = label;
      b.title = what;
      b.tabIndex = -1;
      b.addEventListener("pointerdown", (event) => event.preventDefault());
      b.addEventListener("click", () => {
        run();
        focusScreen();
      });
      bar.appendChild(b);
      return b;
    };
    button("Fullscreen", "The screen alone, with the keys the system would take (Alt-Tab, ...)", fullscreen);
    button("Page to host", "The page of the browser window in front (Firefox, Chromium), opened in this computer's browser (Alt-Shift-O)", sendPage);
    // Files from this computer into the machine's ~/Downloads, as a drop on the screen does: for those who
    // do not think of dragging them there. (The other way: rv-host-save FILE in the machine, or Dolphin's
    // "Save to This Computer".)
    const picker = document.createElement("input");
    picker.type = "file";
    picker.multiple = true;
    picker.style.display = "none";
    picker.addEventListener("change", () => {
      if (picker.files && picker.files.length) {
        hostDrop([...picker.files]);
      }
      picker.value = "";
      focusScreen();
    });
    bar.appendChild(picker);
    button("Files to machine", "Copy files from this computer into the machine's ~/Downloads (dropping them on the screen does the same)", () => picker.click());
    const label = document.createElement("label");
    label.className = "rv-bar-label";
    label.textContent = "Keyboard ";
    layoutSelect = document.createElement("select");
    layoutSelect.className = "rv-bar-select";
    layoutSelect.tabIndex = -1;
    layoutSelect.addEventListener("change", () => {
      setLayout(layoutSelect.value);
      focusScreen();
    });
    label.appendChild(layoutSelect);
    bar.appendChild(label);
    if (isMac) {
      const cmd = document.createElement("label");
      cmd.className = "rv-bar-label";
      const box = document.createElement("input");
      box.type = "checkbox";
      box.tabIndex = -1;
      box.checked = settings.cmdAsCtrl;
      box.addEventListener("change", () => {
        settings.cmdAsCtrl = box.checked;
        saveSettings();
        focusScreen();
      });
      cmd.appendChild(box);
      cmd.appendChild(document.createTextNode(" Cmd is Ctrl"));
      cmd.title = "Cmd-C, Cmd-V, Cmd-Z, ... as Ctrl-C, Ctrl-V, Ctrl-Z (Ctrl-Shift-C, ... in a terminal); other keys with Cmd are Super";
      bar.appendChild(cmd);
    }
    barStatus = document.createElement("span");
    barStatus.className = "rv-bar-status";
    bar.appendChild(barStatus);
  };

  // Once a frame: what the bar shows (the layouts the server has, the one in use).
  let statusText = "", statusUntil = 0;
  const note = (message, ms = 6000) => {
    statusText = message;
    statusUntil = performance.now() + ms;
  };
  const barFrame = (v, t) => {
    if (!layoutSelect) {
      return;
    }
    const layouts = (table.layouts || []).map((l) => l.name + "\t" + l.description).join("\n");
    if (layouts !== layoutsShown) {
      layoutsShown = layouts;
      layoutSelect.textContent = "";
      for (const l of table.layouts || []) {
        const o = document.createElement("option");
        o.value = l.name;
        o.textContent = l.description;
        layoutSelect.appendChild(o);
      }
    }
    const current = text(os.memory.buffer, t + H_KBD_CURRENT).slice(0, 32);
    if (current && layoutSelect.value !== current && document.activeElement !== layoutSelect) {
      layoutSelect.value = current;
    }
    let status = "";
    if (v.getUint32(t + H_KBD_STATUS, true) === 1) {
      status = "no layout “" + layoutName() + "” here";
    }
    if (performance.now() < statusUntil) {
      status = statusText;
    }
    if (barStatus.textContent !== status) {
      barStatus.textContent = status;
    }
  };

  // ---- files dropped on the screen from the host
  //
  // The page cannot write the guest's files; the server can (it is a process of the machine). So a file
  // dropped here is handed to it a piece at a time through a block of its memory (the table's xfer_* words):
  // the page puts a piece there with the file's name and size and waits until the server has written it
  // (into ~/Downloads, under a name of its own choosing if that one is taken). After the last file, if they
  // were dropped onto a window, the server offers that window their file:// URIs as a drop (text/uri-list).
  const MAX_FILE = 512 << 20, MAX_DROP = 1 << 30;
  let dropping = Promise.resolve();

  // Point (clientX, clientY) as the screen's pixels, or [-1, -1] if it is not on the screen.
  const screenPoint = (clientX, clientY) => {
    const box = screen().getBoundingClientRect();
    const x = Math.floor((clientX - box.left) / box.width * screenNow[0]);
    const y = Math.floor((clientY - box.top) / box.height * screenNow[1]);
    return x >= 0 && y >= 0 && x < screenNow[0] && y < screenNow[1] ? [x, y] : [-1, -1];
  };

  const waitAck = (t, serial, ms) => new Promise((resolve) => {
    const began = performance.now();
    const look = () => {
      if (!active || !os) {
        resolve(false);
        return;
      }
      if (new Uint32Array(os.memory.buffer)[(t + H_XFER_ACK) / 4] === serial) {
        resolve(true);
        return;
      }
      if (performance.now() - began > ms) {
        resolve(false);
        return;
      }
      setTimeout(look, 4);
    };
    look();
  });

  // Files (File or Blob objects with a name) to the guest; x, y: where on the screen they were dropped, or -1.
  // Resolves to [{ name, path, status }].
  const hostDrop = (files, x = -1, y = -1) => {
    const run = async () => {
      const results = [];
      if (!active || !tableNow || table.version < 3) {
        note("no window server to take the files");
        return results;
      }
      const t = tableNow.t;
      let total = 0;
      const list = [...files].filter((f) => {
        if (f.size > MAX_FILE || total + f.size > MAX_DROP) {
          results.push({ name: f.name, path: "", status: "too big (at most " + (MAX_FILE >> 20) + " MiB a file, "
                         + (MAX_DROP >> 20) + " MiB a drop)" });
          return false;
        }
        total += f.size;
        return true;
      });
      for (let i = 0; i < list.length; i++) {
        const f = list[i];
        const lastFile = i === list.length - 1;
        let sent = 0, ok = true;
        note("copying " + f.name + " into the machine...", 600000);
        do {
          const v = new DataView(os.memory.buffer);
          const at = v.getUint32(t + H_XFER_LO, true) + v.getUint32(t + H_XFER_HI, true) * 0x100000000;
          const cap = v.getUint32(t + H_XFER_CAP, true);
          let piece;
          try {
            piece = new Uint8Array(await f.slice(sent, Math.min(f.size, sent + cap)).arrayBuffer());
          } catch (error) {
            results.push({ name: f.name, path: "", status: "cannot be read (" + error.message + "; a folder?)" });
            ok = false;
            break;
          }
          const buffer = os.memory.buffer;
          const m = new Uint8Array(buffer), w = new DataView(buffer), w32 = new Int32Array(buffer);
          m.set(piece, at);
          m.fill(0, t + H_XFER_NAME, t + H_XFER_NAME + 256);
          m.set(encoder.encode(f.name || "dropped").subarray(0, 255), t + H_XFER_NAME);
          const last = sent + piece.length >= f.size;
          w.setUint32(t + H_XFER_LEN, piece.length, true);
          w.setUint32(t + H_XFER_TOTAL, f.size, true);
          w.setUint32(t + H_XFER_FLAGS, (sent === 0 ? 1 : 0) | (last ? 2 : 0) | (last && lastFile ? 4 : 0), true);
          w.setInt32(t + H_XFER_X, x, true);
          w.setInt32(t + H_XFER_Y, y, true);
          const serial = (w.getUint32(t + H_XFER_SERIAL, true) + 1) >>> 0;
          Atomics.store(w32, (t + H_XFER_SERIAL) / 4, serial);
          ringBell();
          if (!await waitAck(t, serial, 60000)) {
            results.push({ name: f.name, path: "", status: "the server did not take it" });
            ok = false;
            break;
          }
          const status = new DataView(os.memory.buffer).getUint32(t + H_XFER_STATUS, true);
          if (status) {
            results.push({ name: f.name, path: "", status: "error " + status });
            ok = false;
            break;
          }
          sent += piece.length;
        } while (sent < f.size);
        if (ok) {
          results.push({ name: f.name, path: text(os.memory.buffer, t + H_XFER_PATH).replace(/\0.*$/, "") || "", status: "ok" });
        }
      }
      const done = results.filter((r) => r.status === "ok");
      note(done.length + " of " + results.length + " file" + (results.length === 1 ? "" : "s") + " in ~/Downloads"
           + (results.length > done.length ? " (" + results.filter((r) => r.status !== "ok").map((r) => r.name + ": " + r.status).join("; ") + ")" : ""));
      stats.drops = (stats.drops || []).concat(results).slice(-20);
      return results;
    };
    dropping = dropping.then(run, run);
    return dropping;
  };

  const setupDrop = () => {
    // (A drag in the guest is the guest's: the browser is not to start one of its own from the page's elements
    // under the pointer, a title text or the panel, say.)
    stage.addEventListener("dragstart", (event) => event.preventDefault());
    stage.addEventListener("dragover", (event) => {
      if (active && event.dataTransfer && [...event.dataTransfer.types].includes("Files")) {
        event.preventDefault();
        event.dataTransfer.dropEffect = "copy";
      }
    });
    stage.addEventListener("drop", (event) => {
      if (!active || !event.dataTransfer || !event.dataTransfer.files.length) {
        return;
      }
      event.preventDefault();
      const [x, y] = screenPoint(event.clientX, event.clientY);
      hostDrop(event.dataTransfer.files, x, y);
    });
  };

  // ---- the wheel
  //
  // The runtime's own handler makes one click of the wheel of every wheel event, whatever its size. A mouse's
  // notch is one event, but a trackpad (every Mac's) sends a stream of small ones, 30 to 60 for a short swipe:
  // each was a whole click, three lines, so a swipe threw the text by a hundred lines. Here the events are
  // added up by their distance instead, a click per WHEEL_STEP pixels (three lines of text are about that
  // far), and the first event after a pause or a change of direction makes one by itself: a mouse's notch is
  // one click, whether the browser calls it 4 pixels (macOS) or 100 (Windows, Linux).
  const WHEEL_STEP = 50, WHEEL_PAUSE = 250;
  const wheel = { rest: 0, last: -1e9, dir: 0 };
  const onWheel = (event) => {
    if (!active || !os || !os.screen_input || event.target !== screen()) {
      return;	// (the runtime's handler has it: the console, or no window server)
    }
    event.preventDefault();
    event.stopImmediatePropagation();
    const dy = event.deltaY * (event.deltaMode === 1 ? 40 : event.deltaMode === 2 ? screen().clientHeight : 1);
    const now = performance.now();
    if (!dy) {
      return;
    }
    let clicks = 0;
    if (now - wheel.last > WHEEL_PAUSE || Math.sign(dy) !== wheel.dir) {
      clicks = wheel.dir = Math.sign(dy);
      wheel.rest = 0;
    } else {
      wheel.rest += dy;
    }
    wheel.last = now;
    while (Math.abs(wheel.rest) >= WHEEL_STEP) {
      const c = Math.sign(wheel.rest);
      clicks += c;
      wheel.rest -= c * WHEEL_STEP;
    }
    if (clicks) {
      os.screen_input(3, 0, -clicks);	// (the kernel's wheel: up is positive; the browser's deltaY: down is)
    }
  };

  const setupKeyboard = () => {
    imeSetup();
    window.addEventListener("wheel", onWheel, { capture: true, passive: false });
    window.addEventListener("keydown", onKeyDown, true);
    window.addEventListener("keyup", onKeyUp, true);
    window.addEventListener("blur", lostKeyboard);
    if (screen()) {
      screen().addEventListener("blur", (event) => {
        if (!switcher.open && !(ime.area && event.relatedTarget === ime.area)) {
          lostKeyboard();
        }
      });
    }
    if (ime.area) {
      ime.area.addEventListener("blur", (event) => {
        if (!switcher.open && event.relatedTarget !== screen()) {
          lostKeyboard();	// (the keyboard went somewhere else than between the screen and the input method's field)
        }
      });
    }
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) {
        lostKeyboard();
      }
    });
  };

  const loop = () => {
    try {
      if (!os && window.linuxOs) {
        os = window.linuxOs;
      }
      if (table && gl) {
        frame();
      }
    } catch (error) {
      say("display: " + (error && error.stack || error));
      stop("an error in the draw loop");
    }
    requestAnimationFrame(loop);
  };

  window.rvDisplay = {
    active: () => active,
    clipboard,
    // An order for the server, as the frames send them (README: the orders ring): for scripts and tests.
    order: (op, id, a = 0, b = 0, c = 0) => order(op, id, a, b, c),
    // Files to the guest as if dropped at x, y on the screen (File objects): for scripts and tests.
    drop: (files, x = -1, y = -1) => hostDrop(files, x, y),
    // The layout: set one by name, or read what the server has.
    layout: (name) => {
      if (name) {
        setLayout(name);
      }
      return tableNow && os ? text(os.memory.buffer, tableNow.t + H_KBD_CURRENT).slice(0, 32) : null;
    },
    switcher: () => ({ open: switcher.open, at: switcher.at, items: switcher.items.map((w) => w.title || w.app) }),
    windowList: () => (active ? windowList() : []),
    settings,
    fullscreen,
    // The page of the browser window in front to the host's browser (Alt-Shift-O): for scripts and tests.
    sendPage: () => sendPage(),
    lastSend: () => lastSend,
    screenSize: () => screenNow.slice(),
    get canvas() {
      return canvas;
    },
    // The WebGL2 context the windows are drawn with (a GPU executor on the main thread draws in it too).
    get gl() {
      return gl;
    },
    stats,
    nativeShell: () => nativeShell,
    windows: () => lastWindows.map((s) => ({ id: s.id, kind: s.kind, layer: s.layer, role: s.role, parent: s.parent,
                                             x: s.sx, y: s.sy, w: s.w, h: s.h, z: s.z,
                                             bw: s.bw, bh: s.bh, scale: s.scale, geometry: s.geometry.slice(),
                                             present: s.present, format: s.format, flags: s.flags, damage: s.damage,
                                             title: os ? text(os.memory.buffer, s.at + S_TITLE) : "",
                                             appId: os ? text(os.memory.buffer, s.at + S_APP_ID) : "" })),
    // A page shown in a window (embedFrame): the element for the window whose app id is "rvembed-TOKEN.*",
    // and what to tell it when its window gets the keyboard and loses it. unembed takes it out again.
    embed: (token, el, { focus = () => {}, blur = () => {} } = {}) => {
      embeds.set(String(token), { el, focus, blur, id: 0, focused: false, placed: "" });
    },
    unembed: (token) => {
      const e = embeds.get(String(token));
      if (e) {
        embeds.delete(String(token));
        e.el.remove();
      }
    },
    // The embedded page's window to the front (a click in the page: the page does not see clicks in a frame).
    activateEmbed: (token) => {
      const id = embedId(String(token));
      if (id && !(tableNow && tableNow.v.getUint32(tableNow.t + H_FOCUS, true) === id)) {
        order(ORDER_ACTIVATE, id);
      }
      return id;
    },
    // Alt+Tab pressed in an embedded page: the keyboard comes back to the screen with Alt held (`alt`: which Alt,
    // its event code), and the switcher opens as if the keys had been pressed here; letting go of Alt picks.
    switchFrom: (direction, alt = "AltLeft") => {
      if (!active) {
        return;
      }
      focusScreen();
      phys.add(alt === "AltRight" ? "AltRight" : "AltLeft");
      swallowed.add(alt === "AltRight" ? "AltRight" : "AltLeft");
      if (switcher.open) {
        cycleSwitcher(direction);
      } else {
        openSwitcher(direction, "alt");
      }
    },
    // Alt let go in the embedded page before the keyboard came here: the switcher picks.
    switchEnd: () => {
      phys.delete("AltLeft");
      phys.delete("AltRight");
      swallowed.delete("AltLeft");
      swallowed.delete("AltRight");
      if (switcher.open) {
        closeSwitcher(true);
      }
    },
    // The composed screen, now, as a PNG data URL (drawn again first: the drawing buffer is not kept).
    shot: () => {
      if (!active) {
        return screen().toDataURL("image/png");
      }
      frame();
      return canvas.toDataURL("image/png");
    },
  };

  document.addEventListener("DOMContentLoaded", () => {
    if (setup()) {
      setupChrome();
      setupClipboard();
      setupBar();
      setupPanel();
      setupKeyboard();
      setupDrop();
      requestAnimationFrame(loop);
    }
  });
})();
