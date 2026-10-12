// Blender on the host's GPU, as a window of the machine's desktop (blenderweb/ in the source tree).
//
// `blender-gpu` in the machine (blenderweb/guest/boot.js, a hostnode program) asks for it on the
// BroadcastChannel "rv-blenderweb", with a mark in the machine's memory: only the page whose machine has the
// mark takes it (the way hostopen.js does). It makes a window of its own at the window server, whose app id is
// "rvembed-ID.rv-blender-gpu"; here an iframe with Blender (Puter's blender-wasm, from blenderweb/serve.py) is
// made for it and handed to display.js (rvDisplay.embed), which lays it over that window every frame. So the
// window moves, resizes, stacks, minimizes and switches like the others; the iframe is the inside.
//
// The iframe's address is another SITE than this page's (scheme and registrable domain: 127.0.0.1 for a page
// on localhost and the other way round, locally; on the public server the name rv-site.json gives as "blender",
// under another registrable domain than the page's), so Chrome runs it in a process of its own: its memory and
// its compiled code (V8 gives a process only so much room for compiled Wasm; Blender's engine is 145 MB of Wasm)
// are not the machine's tab's. A subdomain of the page's own domain would be another origin but the same site,
// and Chrome would put it in the page's process (blenderweb/README.md "Why another site"). It is cross-origin
// isolated (Blender's threads need SharedArrayBuffer): this page is (COOP and COEP from the server), the
// frame's server sends COEP require-corp and CORP cross-origin, and the iframe is allowed "cross-origin-isolated".
//
// Where the frame lives, in this order: on a page from localhost or 127.0.0.1, ?blenderweb=PORT (a local server
// on that port, another site than the page's host); the server's rv-site.json "blender" (an https address);
// on a page from localhost or 127.0.0.1, http://127.0.0.1:8040/ (or localhost:8040: blenderweb/serve.py).
// Anywhere else there is no Blender: the program is told it was refused and the person is told why (a note over
// the page), nothing is ever fetched from 127.0.0.1 on a page that is not local.
//
// What goes between them, all of it through this page (postMessage with the frame, checked by source and
// origin; the channel with the machine, checked by the program's id):
//   frame -> page: hello (answered with init: the file to open, the home directory), fs (a file operation on
//     the machine's home: passed on to the program and its answer back), activate (a click in the frame: the
//     window to the front), switch (Alt+Tab in the frame: the switcher), title (Blender's: the window's),
//     status, ready, failed, quit (the person said the window may close).
//   page -> frame: init, fs-answer, focus (the window got the keyboard), close (the window's close button:
//     the frame asks first).
//   program -> page: open, fs-answer, close, gone. page -> program: open-answer, fs, title, quit.
//
// Exposed as window.rvBlenderWeb: sessions() (id, state, title, the last status), port, log.
(() => {
  "use strict";

  const MARK = 32;
  const MAX_SESSIONS = 3;
  const query = new URLSearchParams(location.search);
  const isLocal = location.hostname === "localhost" || location.hostname === "127.0.0.1" || location.hostname === "[::1]";
  const override = query.get("blenderweb") || "";
  const PORT = Number(override) || 8040;
  // The frame's address and its origin; null until configured() has looked (and for good if there is none).
  let frameUrl = null;
  let frameOrigin = null;
  let configuring = null;

  // Is this an address a frame may live at? https, or http on this computer.
  const usable = (text) => {
    try {
      const u = new URL(text, location.href);
      const here = u.hostname === "localhost" || u.hostname === "127.0.0.1" || u.hostname === "[::1]";
      if (u.protocol !== "https:" && !(u.protocol === "http:" && here && isLocal)) {
        return null;
      }
      u.hash = "";
      u.search = "";
      if (!u.pathname.endsWith("/")) {
        u.pathname += "/";
      }
      return u;
    } catch (error) {
      return null;
    }
  };

  // Works out the frame's address once; resolves to true when there is one. (The address is never taken from
  // the page's own address bar except as a port on this computer: a link must not be able to name a site that
  // is then given the machine's files.)
  const configured = () => {
    if (!configuring) {
      configuring = (async () => {
        let u = null;
        if (!(isLocal && Number(override))) {
          try {
            const answer = await fetch("rv-site.json", { cache: "no-store", signal: AbortSignal.timeout(3000) });
            const site = answer.ok ? await answer.json() : {};
            if (typeof site.blender === "string" && site.blender) {
              u = usable(site.blender);
            }
          } catch (error) {
          }
        }
        if (!u && isLocal) {
          // Another site than this page's (the port alone does not make one: sites are scheme and host).
          u = new URL("http://" + (location.hostname === "127.0.0.1" ? "localhost" : "127.0.0.1") + ":" + PORT + "/");
        }
        if (u) {
          frameUrl = u.href;
          frameOrigin = u.origin;
        }
        return !!u;
      })();
    }
    return configuring;
  };

  // A note over the page (a program asked for Blender and there is none): a short sentence, gone after a while.
  let noteTimer = 0;
  const note = (text) => {
    let el = document.getElementById("rv-blenderweb-note");
    if (!el) {
      el = document.createElement("div");
      el.id = "rv-blenderweb-note";
      el.setAttribute("role", "status");
      el.style.cssText = "position: fixed; left: 50%; bottom: 24px; transform: translateX(-50%); max-width: min(560px, 90vw); "
        + "z-index: 2147483000; padding: 10px 16px; border-radius: 6px; background: #2b2b2b; color: #eee; "
        + "font: 14px/1.4 system-ui, sans-serif; box-shadow: 0 4px 18px rgba(0, 0, 0, .5); pointer-events: none;";
      (document.body || document.documentElement).appendChild(el);
    }
    el.textContent = text;
    el.hidden = false;
    clearTimeout(noteTimer);
    noteTimer = setTimeout(() => { el.hidden = true; }, 10000);
  };

  const log = [];
  const remember = (line) => {
    log.push(new Date().toISOString().slice(11, 19) + " " + line);
    if (log.length > 100) {
      log.shift();
    }
    console.log("blenderweb: " + line);
  };

  const sessions = new Map();	// id -> { id, iframe, open, home, state, title, status }
  const channel = window.rvChannel("rv-blenderweb");
  const toProgram = (s, m) => channel.postMessage({ ...m, id: s.id });
  const toFrame = (s, m) => {
    if (s.iframe.contentWindow) {
      s.iframe.contentWindow.postMessage({ rv: "blenderweb", ...m }, frameOrigin);
    }
  };
  const focusScreen = () => {
    const sc = document.getElementById("screen");
    if (sc) {
      sc.focus({ preventScroll: true });
    }
  };

  // Is the mark in this page's machine's memory where the message says? (Else it is another tab's machine.)
  const ours = (m) => {
    const os = window.linuxOs;
    if (!os || !os.memory || !Array.isArray(m.markBytes) || m.markBytes.length !== MARK || m.markBytes[0] !== 0x52 || m.markBytes[1] !== 0x56) {
      return false;
    }
    const mark = Number(m.mark);
    const buffer = os.memory.buffer;
    if (!Number.isSafeInteger(mark) || mark < 0 || mark + MARK > buffer.byteLength) {
      return false;
    }
    const there = new Uint8Array(buffer, mark, MARK);
    for (let i = 0; i < MARK; i++) {
      if (there[i] !== m.markBytes[i]) {
        return false;
      }
    }
    return true;
  };

  const end = (s, why) => {
    if (!sessions.has(s.id)) {
      return;
    }
    sessions.delete(s.id);
    if (window.rvDisplay && window.rvDisplay.unembed) {
      window.rvDisplay.unembed(s.id);
    } else {
      s.iframe.remove();
    }
    remember(s.id + " ended (" + why + ")");
  };

  const open = (m) => {
    if (!frameUrl) {
      return { status: "refused", reason: "this server has no Blender page configured (rv-site.json \"blender\")" };
    }
    if (!window.rvDisplay || !window.rvDisplay.embed) {
      return { status: "refused", reason: "this page has no display.js with embedded windows" };
    }
    if (sessions.size >= MAX_SESSIONS) {
      return { status: "refused", reason: "already " + MAX_SESSIONS + " Blenders open" };
    }
    if (!/^[0-9a-f]{24}$/.test(m.id)) {
      return { status: "refused", reason: "a bad id" };
    }
    const iframe = document.createElement("iframe");
    iframe.className = "rv-embed rv-blenderweb";
    iframe.title = "Blender";
    // (cross-origin-isolated: Blender's threads; fullscreen and the clipboard as Blender's own.)
    iframe.allow = "cross-origin-isolated; fullscreen; clipboard-read; clipboard-write";
    iframe.style.cssText = "border: 0; margin: 0; padding: 0; background: #303030; visibility: hidden; color-scheme: normal;";
    iframe.src = frameUrl + "#parent=" + encodeURIComponent(location.origin);
    const s = { id: m.id, iframe, open: typeof m.open === "string" ? m.open : null, home: typeof m.home === "string" ? m.home : "/root",
                state: "loading", title: "Blender", status: "", started: performance.now(), pid: m.pid };
    sessions.set(s.id, s);
    window.rvDisplay.embed(s.id, iframe, {
      focus: () => {
        if (document.activeElement !== iframe) {
          iframe.focus();
        }
        toFrame(s, { type: "focus" });
      },
      blur: () => {
        if (document.activeElement === iframe) {
          focusScreen();
        }
      },
    });
    remember(s.id + " opened (pid " + m.pid + (s.open ? ", " + s.open : "") + ") at " + frameUrl);
    return { status: "ok" };
  };

  channel.onmessage = async (event) => {
    const m = event.data;
    if (!m || typeof m.id !== "string") {
      return;
    }
    if (m.type === "open") {
      if (!ours(m)) {
        return;
      }
      await configured();
      const answer = open(m);
      if (answer.status !== "ok") {
        remember(m.id + " refused: " + answer.reason);
        if (!frameUrl) {
          note("Blender is not available on this server.");
        }
      }
      channel.postMessage({ type: "open-answer", id: m.id, ...answer });
      return;
    }
    const s = sessions.get(m.id);
    if (!s) {
      return;
    }
    if (m.type === "fs-answer") {
      toFrame(s, { type: "fs-answer", id: m.rid, ok: m.ok, value: m.value, error: m.error, errno: m.errno });
    } else if (m.type === "close") {
      toFrame(s, { type: "close" });
    } else if (m.type === "gone") {
      end(s, "the program ended");
    }
  };

  const OPS = new Set(["stat", "list", "read", "write", "mkdir", "unlink", "rename"]);
  window.addEventListener("message", (event) => {
    if (!frameOrigin || event.origin !== frameOrigin) {
      return;
    }
    const m = event.data;
    if (!m || m.rv !== "blenderweb") {
      return;
    }
    let s = null;
    for (const x of sessions.values()) {
      if (x.iframe.contentWindow === event.source) {
        s = x;
        break;
      }
    }
    if (!s) {
      return;
    }
    switch (m.type) {
    case "hello":
      toFrame(s, { type: "init", open: s.open, home: s.home });
      break;
    case "fs":
      if (OPS.has(m.op) && Number.isSafeInteger(m.id)) {
        toProgram(s, { type: "fs", rid: m.id, op: m.op, path: String(m.path || ""), to: m.to === undefined ? undefined : String(m.to),
                       data: m.op === "write" ? m.data : undefined });
      }
      break;
    case "activate":
      window.rvDisplay.activateEmbed(s.id);
      break;
    case "switch":
      window.rvDisplay.switchFrom(m.direction < 0 ? -1 : 1, m.alt === "AltRight" ? "AltRight" : "AltLeft");
      break;
    case "switch-end":
      window.rvDisplay.switchEnd();
      break;
    case "mem":
      s.mem = { wasm: m.wasm, js: m.js, at: Math.round(performance.now()) };
      break;
    case "title":
      if (typeof m.title === "string") {
        s.title = m.title.slice(0, 200);
        toProgram(s, { type: "title", title: s.title });
      }
      break;
    case "status":
      s.status = String(m.text || "").slice(0, 300);
      break;
    case "ready":
      s.state = "ready";
      remember(s.id + " ready after " + Math.round((performance.now() - s.started) / 1000) + " s");
      break;
    case "failed":
      // (The window stays, showing why; closing it ends the program.)
      s.state = "failed";
      s.status = String(m.why || "").slice(0, 300);
      remember(s.id + " failed: " + s.status);
      break;
    case "quit":
      toProgram(s, { type: "quit" });
      setTimeout(() => end(s, "closed"), 3000);	// (the program's "gone" ends it sooner)
      break;
    case "log":
      s.frameLog = Array.isArray(m.lines) ? m.lines.slice(-200) : [];
      break;
    }
  });

  window.rvBlenderWeb = {
    port: PORT,
    origin: () => frameOrigin,
    url: () => frameUrl,
    configured,
    log,
    sessions: () => [...sessions.values()].map((s) => ({ id: s.id, state: s.state, title: s.title, status: s.status, open: s.open,
                                                        seconds: Math.round((performance.now() - s.started) / 1000) })),
    // The frame's last log lines (asked for, then read from frameLog a moment later): for tests.
    askLog: (count = 50) => {
      for (const s of sessions.values()) {
        toFrame(s, { type: "log", count });
      }
    },
    frameLog: () => [...sessions.values()].map((s) => s.frameLog || []),
    // Blender's memory (its Wasm memory and JavaScript heap, in bytes), asked for, then read a moment later.
    askMem: () => {
      for (const s of sessions.values()) {
        toFrame(s, { type: "mem" });
      }
    },
    mem: () => [...sessions.values()].map((s) => s.mem || null),
  };
})();
