// Input: the keyboard and the pointer, as the kernel has them, given to the
// window they are for.
//
// The page puts what the person does into the kernel's input ring (the
// runtime's screen_input), and the kernel makes evdev events of it on
// /dev/input/event0 (the keyboard: KEY_* codes) and /dev/input/event1 (an
// absolute pointer, 0..32767 on both axes, BTN_* and REL_WHEEL). We read
// both, without taking them from anyone else (no EVIOCGRAB), on every turn.
//
// The pointer goes to the surface under it (popups first, then windows from
// the top, subsurfaces above their parents, each surface's input region
// counting), and stays with the one a button went down on until all are up.
// The keyboard goes to the active window (shell.js: activate; a click
// activates), or to the newest popup that asked for a grab. A grabbing popup
// also keeps the pointer within its client: a press anywhere else closes the
// popups instead of going anywhere.
//
// The keymap is the layout the page asked for (keyboard.js: the keymaps,
// and the modifiers' state for the keymap in use); when the page asks for
// another, every keyboard is sent it. Keys repeat in the client
// (repeat_info), not here.
//
// The pointer's picture is the page's: a shape (wp_cursor_shape_v1, a name
// the page turns into a CSS cursor) or an image (wl_pointer.set_cursor with a
// surface, which the page turns into a CSS url()); table.js: cursor.
(function (ws) {
  "use strict";

  const { send, log, sys1, sys3, sys4, SYS, table, screen, windows, H, global, F } = ws;
  const O_RDONLY = 0, O_RDWR = 2, O_CREAT = 0o100, O_EXCL = 0o200, O_NONBLOCK = 0o4000, O_CLOEXEC = 0o2000000;
  const AT_FDCWD = -100;
  const EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3;
  const REL_HWHEEL = 6, REL_WHEEL = 8, ABS_X = 0, ABS_Y = 1;
  const BTN_LEFT = 0x110, BTN_TASK = 0x117;
  const EVENT_SIZE = 24;	// struct input_event on a 64-bit kernel: timeval (16), type, code (16 bits each), value (32)
  const EVENTS = 64;
  const EVIOCGNAME_64 = 0x80404506;	// _IOC(_IOC_READ, 'E', 0x06, 64): the device's name
  const ABS_MAX = 32767;
  const CAP_POINTER = 1, CAP_KEYBOARD = 2;
  const SHAPE_DEFAULT = 1;

  const now = () => (performance.now() | 0) >>> 0;
  const clientOf = (surface) => surface && surface.object.client;

  // ---- state

  let px = screen.width / 2, py = screen.height / 2;	// the pointer, in screen pixels
  let absX = -1, absY = -1, moved = false, wheel = 0, hwheel = 0;
  let pointerFocus = null, focusX = 0, focusY = 0;	// the surface the pointer is over, and where in it
  const buttons = new Set();		// buttons down
  let consumed = new Set();		// buttons whose press closed popups: their release goes nowhere
  let keyboardFocus = null;		// the surface with the keyboard
  let wantedFocus = null;		// the window shell.js made active
  const keys = new Set();		// keys down
  const kbd = ws.kbd;
  const grabs = [];			// popups that grab, oldest first
  let grab = null;			// a client's move or resize of its window by the pointer
  let refocus = false;
  let cursor = { kind: 1, shape: SHAPE_DEFAULT, surface: null, hotX: 0, hotY: 0, image: 0, cap: 0 };
  let lastShown = null;

  const pointersOf = (client) => client.pointers || (client.pointers = new Set());
  const keyboardsOf = (client) => client.keyboards || (client.keyboards = new Set());

  // ---- where the pointer is

  const surfaceAt = (x, y) => {
    for (let i = windows.length - 1; i >= 0; i--) {
      const s = windows[i];
      if (!s.current.buffer || s.role === "dnd_icon" || ws.inputHidden(s)) {
        continue;
      }
      const [sx, sy] = ws.screenPos(s);
      if (ws.accepts(s, x - sx, y - sy)) {
        return s;
      }
    }
    return null;
  };

  const grabClient = () => grabs.length ? clientOf(grabs[grabs.length - 1]) : null;

  const local = (surface) => {
    const [sx, sy] = ws.screenPos(surface);
    return [px - sx, py - sy];
  };

  const setPointerFocus = (surface) => {
    if (surface === pointerFocus) {
      return;
    }
    const serial = ws.nextSerial();
    if (pointerFocus && !pointerFocus.destroyed) {
      for (const p of pointersOf(clientOf(pointerFocus))) {
        send(p, "leave", serial, pointerFocus.object);
        send(p, "frame");
      }
    }
    pointerFocus = surface;
    if (surface) {
      [focusX, focusY] = local(surface);
      for (const p of pointersOf(clientOf(surface))) {
        send(p, "enter", serial, surface.object, focusX, focusY);
        send(p, "frame");
      }
    }
    // Until the client says otherwise: the arrow.
    showCursor({ kind: 1, shape: SHAPE_DEFAULT });
  };

  // Which surface should have the pointer now, and tell it where the pointer is.
  const pointerMoved = (time) => {
    if (ws.edgeTouched && !buttons.size) {
      ws.edgeTouched(px, py);	// (an auto-hidden panel at that screen edge comes back: layershell.js)
    }
    if (ws.dragging && ws.dragging()) {
      ws.dragMotion(time);	// (drag.js: the pointer is the drag's until the button goes up)
      return;
    }
    if (grab) {
      grabMotion();
      return;
    }
    let target = buttons.size && pointerFocus ? pointerFocus : surfaceAt(px, py);
    const gc = grabClient();
    if (target && gc && clientOf(target) !== gc) {
      target = null;
    }
    setPointerFocus(target);
    if (target) {
      const [x, y] = local(target);
      if (x !== focusX || y !== focusY) {
        focusX = x;
        focusY = y;
        for (const p of pointersOf(clientOf(target))) {
          send(p, "motion", time, x, y);
          send(p, "frame");
        }
      }
    }
  };

  const button = (code, pressed, time) => {
    if (pressed) {
      buttons.add(code);
    } else {
      buttons.delete(code);
    }
    // A window being picked (KWin's interactive selection, which System Settings' "Detect Window Properties"
    // asks for over D-Bus: kwin.js): the press picks the window under the pointer and goes nowhere else.
    if (pressed && ws.picking) {
      const pick = ws.picking;
      ws.picking = null;
      consumed.add(code);
      const hit = surfaceAt(px, py);
      pick(hit ? ws.root(hit) : null);
      return;
    }
    if (ws.dragging && ws.dragging()) {
      if (!pressed && !buttons.size) {
        ws.dragDrop();
        pointerMoved(time);
      }
      return;
    }
    if (grab) {
      if (!pressed && !buttons.size) {
        endGrab();
      }
      return;
    }
    if (pressed) {
      const hit = surfaceAt(px, py);
      const gc = grabClient();
      if (gc && (!hit || clientOf(hit) !== gc)) {
        // A press outside the popups' client: they close, and the press goes nowhere.
        for (let i = grabs.length - 1; i >= 0; i--) {
          ws.dismissPopup(grabs[i]);
        }
        grabs.length = 0;
        consumed.add(code);
        updateKeyboard();
        pointerMoved(time);
        return;
      }
      if (!pointerFocus || buttons.size === 1) {
        setPointerFocus(hit);
      }
      if (hit && !gc) {
        const root = ws.root(hit);
        if (root.top && !(root.flags & F.ACTIVATED)) {
          ws.activate(root);
        } else if (root.layer && ws.layerPressed) {
          ws.layerPressed(root);	// (a layer surface that takes the keyboard on demand gets it: layershell.js)
        }
      }
    } else if (consumed.has(code)) {
      consumed.delete(code);
      return;
    }
    if (pointerFocus) {
      const serial = ws.nextSerial();
      if (pressed) {
        ws.lastPressSerial = serial;
        ws.lastPressSurface = pointerFocus;	// (shell.js: the parent a popup without one is most likely to mean)
      }
      for (const p of pointersOf(clientOf(pointerFocus))) {
        send(p, "button", serial, time, code, pressed ? 1 : 0);
        send(p, "frame");
      }
    }
    if (!pressed && !buttons.size) {
      pointerMoved(time);	// (the surface under the pointer may be another now)
    }
  };

  const axis = (vertical, horizontal, time) => {
    if (!pointerFocus || grab) {
      return;
    }
    for (const p of pointersOf(clientOf(pointerFocus))) {
      send(p, "axis_source", 0);	// a wheel
      if (vertical) {
        send(p, "axis_discrete", 0, -vertical);
        send(p, "axis", time, 0, -vertical * 15);
      }
      if (horizontal) {
        send(p, "axis_discrete", 1, horizontal);
        send(p, "axis", time, 1, horizontal * 15);
      }
      send(p, "frame");
    }
  };

  // ---- a client moving or resizing its own window by the pointer (xdg_toplevel.move/resize)

  ws.startGrab = (surface, kind, edges) => {
    surface = ws.root(surface);
    if (!buttons.size || !surface.top || (surface.flags & (F.MAXIMIZED | F.FULLSCREEN))) {
      return;
    }
    const g = ws.geometry(surface);
    grab = { surface, kind, edges, x: px, y: py, wx: surface.top.wx, wy: surface.top.wy, w: g[2], h: g[3] };
    setPointerFocus(null);
    showCursor({ kind: 1, shape: kind === "move" ? 17 : resizeShape(edges) });	// grabbing, or *-resize
  };

  const resizeShape = (edges) => ({ 1: 19, 2: 22, 4: 25, 8: 18, 5: 21, 9: 20, 6: 24, 10: 23 })[edges] || SHAPE_DEFAULT;

  const grabMotion = () => {
    const g = grab, dx = Math.round(px - g.x), dy = Math.round(py - g.y);
    if (g.surface.destroyed || !g.surface.top) {
      grab = null;
      return;
    }
    if (g.kind === "move") {
      ws.moveWindow(g.surface, g.wx + dx, g.wy + dy);
    } else {
      let w = g.w, h = g.h;
      if (g.edges & 8) {
        w += dx;
      } else if (g.edges & 4) {
        w -= dx;
      }
      if (g.edges & 2) {
        h += dy;
      } else if (g.edges & 1) {
        h -= dy;
      }
      ws.resizeWindow(g.surface, g.edges, w, h);
    }
  };

  const endGrab = () => {
    if (grab && grab.kind === "resize" && grab.surface.top) {
      ws.resizeEnd(grab.surface);
    }
    grab = null;
    pointerMoved(now());
  };

  // ---- the keyboard

  const sendModifiers = (client) => {
    const serial = ws.nextSerial();
    for (const k of keyboardsOf(client)) {
      send(k, "modifiers", serial, kbd.depressed, kbd.latched, kbd.locked, kbd.group);
    }
  };

  const pressedKeys = () => {
    const a = new Uint8Array(4 * keys.size);
    const v = new DataView(a.buffer);
    let i = 0;
    for (const k of keys) {
      v.setUint32(4 * i++, k, true);
    }
    return a;
  };

  const key = (code, value, time) => {
    if (value === 2) {
      return;	// (the kernel's repeat: the clients repeat by themselves)
    }
    const pressed = value === 1;
    if (pressed) {
      keys.add(code);
    } else {
      keys.delete(code);
    }
    const mods = kbd.key(code, pressed);
    if (keyboardFocus) {
      const client = clientOf(keyboardFocus);
      const serial = ws.nextSerial();
      for (const k of keyboardsOf(client)) {
        send(k, "key", serial, time, code, pressed ? 1 : 0);
      }
      if (mods) {
        sendModifiers(client);
      }
    }
  };

  const setKeyboard = (surface) => {
    if (surface === keyboardFocus) {
      return;
    }
    const serial = ws.nextSerial();
    if (keyboardFocus && !keyboardFocus.destroyed) {
      for (const k of keyboardsOf(clientOf(keyboardFocus))) {
        send(k, "leave", serial, keyboardFocus.object);
      }
    }
    const before = keyboardFocus;
    keyboardFocus = surface;
    if (surface) {
      const client = clientOf(surface);
      if (ws.selectionsTo && (!before || clientOf(before) !== client)) {
        ws.selectionsTo(client);	// (data.js: what is on the clipboard, before the keyboard comes)
      }
      for (const k of keyboardsOf(client)) {
        send(k, "enter", serial, surface.object, pressedKeys());
      }
      sendModifiers(client);
    }
    table.focus(surface ? ws.root(surface).id : 0);
    if (ws.textInputFocus) {
      ws.textInputFocus(surface);	// (textinput.js: the text inputs follow the keyboard)
    }
  };

  const updateKeyboard = () => {
    let target = wantedFocus;
    for (let i = grabs.length - 1; i >= 0; i--) {
      if (grabs[i].slot >= 0) {
        target = grabs[i];
        break;
      }
    }
    if (target && target.destroyed) {
      target = null;
    }
    setKeyboard(target);
  };

  ws.setKeyboardFocus = (surface) => {
    wantedFocus = surface;
    updateKeyboard();
  };

  ws.keyboardClient = () => keyboardFocus ? clientOf(keyboardFocus) : null;

  ws.popupGrab = (surface) => {
    if (!grabs.includes(surface)) {
      grabs.push(surface);
    }
    updateKeyboard();
    refocus = true;
  };

  ws.popupGone = (surface) => {
    const at = grabs.indexOf(surface);
    if (at >= 0) {
      grabs.splice(at, 1);
    }
    updateKeyboard();
    refocus = true;
  };

  ws.surfaceGone = (surface) => {
    if (ws.dragSurfaceGone) {
      ws.dragSurfaceGone(surface);
    }
    if (pointerFocus === surface) {
      pointerFocus = null;
      refocus = true;
    }
    if (keyboardFocus === surface) {
      keyboardFocus = null;
      table.focus(0);
    }
    if (wantedFocus === surface) {
      wantedFocus = null;
    }
    if (cursor.surface === surface) {
      cursor.surface = null;
    }
    const at = grabs.indexOf(surface);
    if (at >= 0) {
      grabs.splice(at, 1);
      updateKeyboard();
    }
    if (grab && grab.surface === surface) {
      grab = null;
    }
  };

  // Windows came, went or moved: the surface under the pointer may be another (looked at on the next turn).
  ws.seatChanged = () => {
    refocus = true;
  };

  // ---- the pointer's picture

  const showCursor = (c) => {
    const key = c.kind + ":" + (c.shape || 0) + ":" + (c.image || 0) + ":" + (c.serial || 0);
    if (key === lastShown) {
      return;
    }
    lastShown = key;
    table.cursor(c.kind, c.shape, c.image, c.w, c.h, c.stride, c.hotX, c.hotY);
  };

  let imageSerial = 0;
  // The cursor surface's current buffer, copied (it may be in a pool we could not map: ws.bufferAddress then
  // reads it from the file), for the page.
  const showCursorSurface = (surface) => {
    const b = surface.current.buffer;
    if (!b) {
      showCursor({ kind: 0 });
      return;
    }
    const bytes = b.width * 4 * b.height;
    if (bytes > cursor.cap) {
      if (cursor.image) {
        ws.free(cursor.image);
      }
      cursor.cap = Math.max(bytes, 64 * 64 * 4);
      cursor.image = ws.alloc(cursor.cap);
    }
    const src = ws.bufferAddress(b);
    const m = ws.u8();
    for (let row = 0; row < b.height; row++) {
      m.copyWithin(cursor.image + row * b.width * 4, src + row * b.stride, src + row * b.stride + b.width * 4);
    }
    showCursor({ kind: 2, image: cursor.image, w: b.width, h: b.height, stride: b.width * 4,
                 hotX: cursor.hotX, hotY: cursor.hotY, serial: ++imageSerial });
  };

  // From compositor.js: a commit to a surface that is (or was) a pointer's picture.
  ws.cursorCommit = (surface, st) => {
    if (st.offset) {
      cursor.hotX -= st.offset[0];
      cursor.hotY -= st.offset[1];
    }
    if (cursor.surface === surface && pointerFocus && clientOf(pointerFocus) === clientOf(surface)) {
      showCursorSurface(surface);
    }
    const b = surface.current.buffer;
    if (b && surface.busy.has(b)) {
      surface.busy.delete(b);
      send(b, "release");	// (copied already)
    }
    surface.waiting.push(...st.frames);
  };

  // ---- the objects

  H.wl_seat = {
    get_pointer(seat, id) {
      const object = ws.create(seat.client, id, "wl_pointer", seat.version);
      pointersOf(seat.client).add(object);
      object.onDestroy = () => pointersOf(seat.client).delete(object);
      if (pointerFocus && clientOf(pointerFocus) === seat.client) {
        send(object, "enter", ws.nextSerial(), pointerFocus.object, focusX, focusY);
        send(object, "frame");
      }
    },
    get_keyboard(seat, id) {
      const object = ws.create(seat.client, id, "wl_keyboard", seat.version);
      keyboardsOf(seat.client).add(object);
      object.onDestroy = () => keyboardsOf(seat.client).delete(object);
      if (kbd.keymap) {
        send(object, "keymap", 1, kbd.keymap.fd, kbd.keymap.size);	// (xkb_v1)
      }
      send(object, "repeat_info", 25, 600);
      if (keyboardFocus && clientOf(keyboardFocus) === seat.client) {
        send(object, "enter", ws.nextSerial(), keyboardFocus.object, pressedKeys());
        send(object, "modifiers", ws.nextSerial(), kbd.depressed, kbd.latched, kbd.locked, kbd.group);
      }
    },
    get_touch(seat, id) {
      ws.create(seat.client, id, "wl_touch", seat.version);
    },
    release() {},
  };

  H.wl_pointer = {
    set_cursor(pointer, serial, surfaceObject, hotX, hotY) {
      if (!pointerFocus || clientOf(pointerFocus) !== pointer.client) {
        return;
      }
      if (!surfaceObject) {
        cursor.surface = null;
        showCursor({ kind: 0 });
        return;
      }
      const surface = surfaceObject.surface;
      if (surface.role && surface.role !== "cursor") {
        ws.error(pointer.client, pointer, 0, "set_cursor: the surface has another role");
        return;
      }
      surface.role = "cursor";
      cursor.surface = surface;
      cursor.hotX = hotX;
      cursor.hotY = hotY;
      showCursorSurface(surface);
    },
    release() {},
  };

  H.wl_keyboard = {
    release() {},
  };

  H.wl_touch = {
    release() {},
  };

  H.wp_cursor_shape_manager_v1 = {
    destroy() {},
    get_pointer(manager, id, pointer) {
      const object = ws.create(manager.client, id, "wp_cursor_shape_device_v1", manager.version);
      object.pointer = pointer;
    },
    get_tablet_tool_v2(manager, id) {
      ws.create(manager.client, id, "wp_cursor_shape_device_v1", manager.version);
    },
  };

  H.wp_cursor_shape_device_v1 = {
    destroy() {},
    set_shape(device, serial, shape) {
      if (!pointerFocus || clientOf(pointerFocus) !== device.client || grab) {
        return;
      }
      cursor.surface = null;
      showCursor({ kind: 1, shape });
    },
  };

  global("wl_seat", 7, (seat) => {
    send(seat, "capabilities", CAP_POINTER | CAP_KEYBOARD);
    send(seat, "name", "seat0");
  });
  global("wp_cursor_shape_manager_v1", 1, () => {});

  // ---- the devices

  const events = ws.alloc(EVENT_SIZE * EVENTS);

  const readDevice = (fd) => {
    for (;;) {
      const n = sys3(SYS.read, fd, events, EVENT_SIZE * EVENTS);
      if (n === -4) {
        continue;
      }
      if (n <= 0) {
        return;
      }
      const v = ws.dv();
      for (let at = events; at + EVENT_SIZE <= events + n; at += EVENT_SIZE) {
        const type = v.getUint16(at + 16, true), code = v.getUint16(at + 18, true), value = v.getInt32(at + 20, true);
        input(type, code, value);
      }
      if (n < EVENT_SIZE * EVENTS) {
        return;
      }
    }
  };

  const flushMotion = (time) => {
    if (moved) {
      moved = false;
      if (absX >= 0) {
        px = Math.min(screen.width - 1, Math.round(absX * screen.width / ABS_MAX));
      }
      if (absY >= 0) {
        py = Math.min(screen.height - 1, Math.round(absY * screen.height / ABS_MAX));
      }
      pointerMoved(time);
    }
  };

  const input = (type, code, value) => {
    const time = now();
    if (type === EV_ABS) {
      if (code === ABS_X) {
        absX = value;
        moved = true;
      } else if (code === ABS_Y) {
        absY = value;
        moved = true;
      }
    } else if (type === EV_REL) {
      if (code === REL_WHEEL) {
        wheel += value;
      } else if (code === REL_HWHEEL) {
        hwheel += value;
      }
    } else if (type === EV_KEY) {
      if (code >= BTN_LEFT && code <= BTN_TASK) {
        flushMotion(time);
        button(code, value !== 0, time);
      } else {
        key(code, value, time);
      }
    } else if (type === EV_SYN) {
      flushMotion(time);
      if (wheel || hwheel) {
        axis(wheel, hwheel, time);
        wheel = 0;
        hwheel = 0;
      }
    }
  };

  // Called by server.js once its epoll is there: the devices to watch.
  ws.seatStart = () => {
    showCursor({ kind: 1, shape: SHAPE_DEFAULT });	// (the arrow, until a client says otherwise)
    for (const name of ["/dev/input/event0", "/dev/input/event1"]) {
      const fd = sys4(SYS.openat, AT_FDCWD, ws.path(name, ws.P0), O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
      if (fd < 0) {
        log("seat: cannot open " + name + " (error " + -fd + "): no input from it");
        continue;
      }
      ws.watchFd(fd, () => readDevice(fd));
    }
    // The page's doorbell (the kernel's "Wasm host" device, an EV_MSC event each time the page rings): what it
    // writes into the table is then seen at once, and the turns need not come often (server.js). A kernel
    // without that device has no event2, or an event2 that is something else: then the turns stay short.
    const bell = sys4(SYS.openat, AT_FDCWD, ws.path("/dev/input/event2", ws.P0), O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
    if (bell >= 0) {
      const name = ws.alloc(64);
      ws.u8().fill(0, name, name + 64);
      const n = sys3(SYS.ioctl, bell, EVIOCGNAME_64, name);
      const named = n > 0 && ws.cstring(name) === "Wasm host";
      ws.free(name);
      if (named) {
        ws.doorbell = true;
        ws.watchFd(bell, () => {
          while (sys3(SYS.read, bell, events, EVENT_SIZE * EVENTS) === EVENT_SIZE * EVENTS) {
          }
        });
        log("seat: the page's doorbell is there");
      } else {
        sys1(SYS.close, bell);
      }
    }
  };

  // Every turn (server.js): has the page asked for another layout? Then every keyboard is sent its keymap
  // (and the one with the focus the modifiers again: the old state means nothing in the new keymap).
  ws.keyboardTurn = () => {
    const want = table.layout();
    if (!want) {
      return;
    }
    const ok = want.name === (kbd.keymap && kbd.keymap.name) || kbd.use(want.name);
    if (ok) {
      for (const client of ws.clients()) {
        for (const k of keyboardsOf(client)) {
          send(k, "keymap", 1, kbd.keymap.fd, kbd.keymap.size);
        }
      }
      if (keyboardFocus) {
        sendModifiers(clientOf(keyboardFocus));
      }
    } else {
      log("seat: no layout " + JSON.stringify(want.name) + " here; the keyboard stays " + (kbd.keymap && kbd.keymap.name));
    }
    table.layoutDone(want.serial, ok ? 0 : 1, kbd.keymap ? kbd.keymap.name : "");
  };

  // Every turn (server.js): what has to be looked at again.
  ws.seatTurn = () => {
    if (px >= screen.width || py >= screen.height) {
      px = Math.min(px, screen.width - 1);	// (the screen got smaller)
      py = Math.min(py, screen.height - 1);
    }
    updateKeyboard();
    if (refocus && !buttons.size && !grab) {
      refocus = false;
      pointerMoved(now());
    }
  };

  // For drag.js: where the pointer is, the surface under a point, and the pointer taken from its client.
  ws.pointerState = () => ({ x: px, y: py, buttons: buttons.size });
  ws.surfaceAt = surfaceAt;
  ws.leavePointer = () => setPointerFocus(null);

  // For tests (tools/wl-dump.js reads the log): where things are.
  ws.pointerAt = () => [px, py];
  ws.pointerFocus = () => pointerFocus;
  ws.keyboardFocus = () => keyboardFocus;
  ws.seatState = () => ({ px, py, pointer: pointerFocus && pointerFocus.id, keyboard: keyboardFocus && keyboardFocus.id,
                          grabs: grabs.map((s) => s.id) });
})
