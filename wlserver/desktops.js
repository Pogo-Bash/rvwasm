// Virtual desktops, as KWin has them (kwin 6.3, src/virtualdesktops.cpp):
// a list of desktops, each with an id and a name; a current one; a grid of
// `rows` rows that left, right, up and down move in (wrapping around when
// kwinrc [Windows] RollOverDesktops says so; KWin's default is not to); every window on
// one desktop or on all of them.
//
// Where they come from and go to: kwinrc [Desktops] Number, Rows, Name_N and
// Id_N, read at start and again when KWin would read them (kwin.js: the
// reconfigure method and the reloadConfig signal System Settings sends), and
// written back as KWin writes them whenever they change here (a name is only
// written when it is not the default "Desktop N"). Who changes them: the page
// (orders 11 to 20, PLASMA-PROTOCOL.md), KDE's tools over D-Bus (kwin.js),
// X pagers (xwm.js). Everyone who shows them is told: the page through the
// table (the desktops words and the JSON list), D-Bus listeners (kwin.js's
// signals), X programs (the root's _NET_ properties, xwm.js).
//
// A window's desktop is kept as the desktop's id in its top (top.vd), or null
// for all desktops; a window gets the current desktop when it first maps
// (unless a window rule says otherwise: rules.js). A window not on the
// current desktop is "away": not drawn, no input (shell.js: windowAway), and
// its slot says so (flag OTHER_DESKTOP, compositor.js: writeSlot).
(function (ws) {
  "use strict";

  const { log, table, windows, F } = ws;
  const K = ws.kconfig;
  const MAX = 20;	// (KWin's VirtualDesktopManager::maximum())
  // The page's orders (PLASMA-PROTOCOL.md); desktops are 1-based there.
  const ORDER_ALL_DESKTOPS = 14, ORDER_WINDOW_TO_DESKTOP = 15, ORDER_SET_DESKTOP = 16, ORDER_DESKTOP_COUNT = 19,
        ORDER_SHOW_DESKTOP = 20, ORDER_DESKTOP_STEP = 21;
  const onOff = (a, now) => (a === 2 ? !now : a !== 0);	// (a: 1 set, 0 clear, 2 toggle)
  const LEFT = 1, RIGHT = 2, UP = 3, DOWN = 4, NEXT = 5, PREVIOUS = 6;

  const uuid = () => {
    const b = new Uint8Array(16);
    crypto.getRandomValues(b);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    const h = [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
    return h.slice(0, 8) + "-" + h.slice(8, 12) + "-" + h.slice(12, 16) + "-" + h.slice(16, 20) + "-" + h.slice(20);
  };
  const defaultName = (n) => "Desktop " + n;

  const state = { list: [], current: 0, rows: 2, wrap: false, showing: false, shown: [] };
  const lastActive = new Map();	// desktop id -> the window that was active there when it was left (KWin's focus chain, in short)

  // ---- kwinrc

  const load = () => {
    const cfg = K.read("kwinrc");
    const n = Math.max(1, Math.min(MAX, K.int(cfg, "Desktops", "Number", 1)));
    const old = state.list;
    const list = [];
    for (let i = 1; i <= n; i++) {
      const id = K.get(cfg, "Desktops", "Id_" + i, null) || (old[i - 1] && old[i - 1].id) || uuid();
      list.push({ id, name: K.get(cfg, "Desktops", "Name_" + i, null) || defaultName(i) });
    }
    const currentId = old[state.current] && old[state.current].id;
    state.list = list;
    state.rows = Math.max(1, Math.min(n, K.int(cfg, "Desktops", "Rows", 2)));
    state.wrap = K.bool(cfg, "Windows", "RollOverDesktops", false);	// (kwin.kcfg: false)
    const at = list.findIndex((d) => d.id === currentId);
    state.current = at >= 0 ? at : Math.min(state.current, n - 1);
    // Windows on a desktop that is no more go to the last one (they are not lost).
    for (const w of windows) {
      if (w.top && w.top.vd && !list.some((d) => d.id === w.top.vd)) {
        w.top.vd = list[list.length - 1].id;
      }
    }
    // Ids made up just now are kept, as KWin keeps them.
    if (list.some((d, i) => K.get(cfg, "Desktops", "Id_" + (i + 1), null) !== d.id)) {
      save();
    }
  };

  // As KWin's VirtualDesktopManager::save(): Number, Id_N, Name_N where it is not the default, Rows; keys of
  // desktops beyond the number are taken out.
  const save = () => {
    const cfg = K.readFile("kwinrc");
    const g = "Desktops";
    for (let i = state.list.length + 1; K.get(cfg, g, "Id_" + i, null) !== null || K.get(cfg, g, "Name_" + i, null) !== null; i++) {
      K.remove(cfg, g, "Id_" + i);
      K.remove(cfg, g, "Name_" + i);
    }
    K.set(cfg, g, "Number", state.list.length);
    state.list.forEach((d, i) => {
      if (d.name !== defaultName(i + 1)) {
        K.set(cfg, g, "Name_" + (i + 1), d.name);
      } else {
        K.remove(cfg, g, "Name_" + (i + 1));
      }
      K.set(cfg, g, "Id_" + (i + 1), d.id);
    });
    K.set(cfg, g, "Rows", state.rows);
    K.write("kwinrc", cfg);
  };

  // ---- telling everyone

  let lastPublished = "";
  const publish = () => {
    const json = JSON.stringify(state.list.map((d) => ({ id: d.id, name: d.name })));
    const key = json + state.current + "," + state.rows + "," + state.showing;
    if (key === lastPublished) {
      return;
    }
    lastPublished = key;
    // (one line per change, so that a run's log says what the page was told)
    log("desktops: " + state.list.length + " in " + state.rows + " rows, current " + (state.current + 1) + (state.showing ? ", showing the desktop" : ""));
    table.desktops(state.current, state.list.length, state.rows, new TextEncoder().encode(json), state.showing);
    if (ws.xwmDesktops) {
      ws.xwmDesktops();
    }
  };

  // Everything drawn is to be written again (which windows are away changed), the pointer looked at again.
  const redraw = () => {
    for (const w of windows) {
      w.dirty = true;
    }
    if (ws.seatChanged) {
      ws.seatChanged();
    }
  };

  // ---- the questions others ask

  const indexOf = (id) => state.list.findIndex((d) => d.id === id);

  // A window's desktop, 0-based, or -1 for all.
  ws.windowDesktop = (surface) => {
    const t = surface.top;
    if (!t || t.vd === null) {
      return t ? -1 : 0;
    }
    const i = t.vd === undefined ? state.current : indexOf(t.vd);
    return i < 0 ? state.current : i;
  };
  ws.onCurrentDesktop = (surface) => {
    const t = surface.top;
    if (!t || t.vd === null || t.vd === undefined) {
      return true;
    }
    const i = indexOf(t.vd);
    return i < 0 || i === state.current;
  };
  ws.desktops = state;

  // ---- changing them

  // The window that has the keyboard after a switch: the topmost window of the new desktop (KWin's focus chain is
  // by recency; the stacking order is near enough), or none.
  const refocus = (keep) => {
    const active = windows.find((w) => w.top && ws.isWindow(w) && (w.flags & F.ACTIVATED));
    if (keep) {
      return;
    }
    if (active && !ws.windowAway(active)) {
      return;
    }
    if (active) {
      active.flags &= ~F.ACTIVATED;
      active.dirty = true;
      ws.reconfigure(active);
    }
    // The window that was active on this desktop when it was left, if it is still here; else the topmost.
    const back = lastActive.get(state.list[state.current].id);
    if (back && windows.includes(back) && ws.isWindow(back) && !ws.windowAway(back)) {
      ws.activate(back);
    } else {
      ws.activateNext(null);
    }
  };

  // Popups of windows that went away are closed (their menus would hang in the air).
  const closeStrayPopups = () => {
    for (const w of [...windows]) {
      if (w.xdg && w.xdg.popup && ws.windowAway(ws.root(w))) {
        ws.dismissPopup(w);
      }
    }
  };

  // Make desktop `i` the current one. `keep`: a window that is about to be activated anyway (no other is).
  const setCurrent = (i, keep = null) => {
    i = Math.max(0, Math.min(state.list.length - 1, i | 0));
    if (i === state.current) {
      return false;
    }
    const before = state.current;
    const active = windows.find((w) => w.top && ws.isWindow(w) && (w.flags & F.ACTIVATED));
    if (active) {
      lastActive.set(state.list[before].id, active);
    }
    state.current = i;
    if (state.showing) {
      setShowing(false);
    }
    closeStrayPopups();
    redraw();
    refocus(keep);
    publish();
    if (ws.kwinDesktopsChanged) {
      ws.kwinDesktopsChanged("current", { before });
    }
    return true;
  };
  ws.setCurrentDesktop = setCurrent;
  ws.toWindowsDesktop = (surface) => {
    const i = surface.top && surface.top.vd ? indexOf(surface.top.vd) : -1;
    if (i >= 0) {
      setCurrent(i, surface);
    }
  };

  // The grid: column and row of desktop i; the desktop at (column, row) or -1.
  const columns = () => Math.ceil(state.list.length / state.rows);
  const cell = (i) => [i % columns(), Math.floor(i / columns())];
  const at = (x, y) => {
    const i = y * columns() + x;
    return x >= 0 && y >= 0 && x < columns() && i < state.list.length ? i : -1;
  };

  // The desktop one step away from `from` in a direction (KWin's above/below/toLeft/toRight/next/previous).
  const step = (from, direction, wrap = state.wrap) => {
    const n = state.list.length;
    if (direction === NEXT) {
      return from + 1 < n ? from + 1 : wrap ? 0 : from;
    }
    if (direction === PREVIOUS) {
      return from > 0 ? from - 1 : wrap ? n - 1 : from;
    }
    let [x, y] = cell(from);
    const w = columns(), h = state.rows;
    for (let tries = 0; tries < w * h + 1; tries++) {
      if (direction === LEFT) {
        x--;
      } else if (direction === RIGHT) {
        x++;
      } else if (direction === UP) {
        y--;
      } else if (direction === DOWN) {
        y++;
      }
      if (x < 0 || x >= w || y < 0 || y >= h) {
        if (!wrap) {
          return from;
        }
        x = (x + w) % w;
        y = (y + h) % h;
      }
      const i = at(x, y);
      if (i >= 0) {
        return i;
      }
    }
    return from;
  };
  ws.desktopStep = step;

  // Put a window on desktop i (0-based), or on all (-1).
  const moveWindow = (surface, i) => {
    const t = surface.top;
    if (!t) {
      return;
    }
    const vd = i < 0 ? null : state.list[Math.max(0, Math.min(state.list.length - 1, i))].id;
    if (t.vd === vd) {
      return;
    }
    t.vd = vd;
    ws.dirtyTree(surface);
    if (ws.windowAway(surface)) {
      closeStrayPopups();
      if (surface.flags & F.ACTIVATED) {
        refocus(null);
      }
    }
    if (ws.seatChanged) {
      ws.seatChanged();
    }
    if (ws.statesChanged) {
      ws.statesChanged(surface);
    }
  };
  ws.windowToDesktop = moveWindow;

  // A new desktop at `position` (0-based; clamped), named `name` or "Desktop N". Returns it, or null at the most.
  const create = (position, name) => {
    if (state.list.length >= MAX) {
      return null;
    }
    position = Math.max(0, Math.min(state.list.length, position | 0));
    const d = { id: uuid(), name: name || defaultName(position + 1) };
    const currentId = state.list[state.current].id;
    state.list.splice(position, 0, d);
    state.current = indexOf(currentId);
    state.rows = Math.min(state.rows, state.list.length);
    save();
    redraw();
    publish();
    if (ws.kwinDesktopsChanged) {
      ws.kwinDesktopsChanged("created", { desktop: d, position });
    }
    return d;
  };
  ws.createDesktop = create;

  // Take desktop `id` away (not the last one); its windows go to the desktop before it (or the first).
  const remove = (id) => {
    const i = indexOf(id);
    if (i < 0 || state.list.length <= 1) {
      return false;
    }
    const [gone] = state.list.splice(i, 1);
    const heir = state.list[Math.max(0, i - 1)];
    for (const w of windows) {
      if (w.top && w.top.vd === gone.id) {
        w.top.vd = heir.id;
      }
    }
    let currentChanged = false;
    if (state.current === i) {
      state.current = Math.min(i, state.list.length - 1);	// (KWin: the one that took its place, or the last)
      currentChanged = true;
    } else if (state.current > i) {
      state.current--;
    }
    state.rows = Math.max(1, Math.min(state.rows, state.list.length));
    save();
    closeStrayPopups();
    redraw();
    refocus(null);
    publish();
    if (ws.kwinDesktopsChanged) {
      ws.kwinDesktopsChanged("removed", { desktop: gone, from: i });
      if (currentChanged) {
        ws.kwinDesktopsChanged("current", {});
      }
    }
    return true;
  };
  ws.removeDesktop = remove;

  const rename = (id, name) => {
    const i = indexOf(id);
    if (i < 0 || state.list[i].name === name) {
      return false;
    }
    state.list[i].name = name;
    save();
    publish();
    if (ws.kwinDesktopsChanged) {
      ws.kwinDesktopsChanged("renamed", { desktop: state.list[i], position: i });
    }
    return true;
  };
  ws.renameDesktop = rename;

  // The number of desktops (as an X pager's _NET_NUMBER_OF_DESKTOPS asks): added at the end or taken from it.
  ws.setDesktopCount = (n) => {
    n = Math.max(1, Math.min(MAX, n | 0));
    while (state.list.length < n) {
      create(state.list.length, "");
    }
    while (state.list.length > n) {
      remove(state.list[state.list.length - 1].id);
    }
  };

  const setRows = (rows) => {
    rows = Math.max(1, Math.min(state.list.length, rows | 0));
    if (rows === state.rows) {
      return false;
    }
    state.rows = rows;
    save();
    publish();
    if (ws.kwinDesktopsChanged) {
      ws.kwinDesktopsChanged("rows", {});
    }
    return true;
  };
  ws.setDesktopRows = setRows;

  // "Showing the desktop" (KWin's showDesktop): the windows of the current desktop are minimized, and brought
  // back when it ends, unless one was activated meanwhile (then that one only comes back).
  let entering = false;
  const setShowing = (on) => {
    on = on === null ? !state.showing : !!on;
    if (on === state.showing) {
      return;
    }
    state.showing = on;
    if (on) {
      state.shown = windows.filter((w) => ws.isWindow(w) && !ws.windowAway(w));
      entering = true;	// (minimizing the active one activates the next, which must not end it)
      try {
        for (const w of state.shown) {
          ws.minimize(w);
        }
      } finally {
        entering = false;
      }
    } else {
      const back = state.shown;
      state.shown = [];
      for (const w of back) {
        if (w.top && (w.flags & F.MINIMIZED)) {
          w.flags &= ~F.MINIMIZED;
          w.dirty = true;
        }
      }
      const top = windows.filter((w) => ws.isWindow(w) && !ws.windowAway(w)).pop();
      if (top) {
        ws.activate(top);
      }
    }
    publish();
    if (ws.kwinDesktopsChanged) {
      ws.kwinDesktopsChanged("showing", {});
    }
  };
  ws.setShowingDesktop = setShowing;
  // From shell.js's activate: a window activated while the desktop is shown ends it, and only that one is back.
  ws.showingDesktopEnd = (surface) => {
    if (state.showing && !entering) {
      state.showing = false;
      state.shown = [];
      publish();
      if (ws.kwinDesktopsChanged) {
        ws.kwinDesktopsChanged("showing", {});
      }
    }
  };

  // ---- the page's orders (shell.js hands over the ones it does not know); true if it was one of ours

  ws.desktopOrder = (o, surface) => {
    switch (o.op) {
    case ORDER_SET_DESKTOP:
      setCurrent(o.a - 1);
      return true;
    case ORDER_WINDOW_TO_DESKTOP:
      if (surface) {
        moveWindow(surface, o.a <= 0 ? -1 : o.a - 1);
        if (o.b && o.a > 0) {
          setCurrent(o.a - 1, surface);	// (b: 1 = follow it there)
          ws.activate(surface);
        }
      }
      return true;
    case ORDER_ALL_DESKTOPS:
      if (surface) {
        moveWindow(surface, onOff(o.a, surface.top.vd === null) ? -1 : state.current);
      }
      return true;
    case ORDER_DESKTOP_COUNT:
      ws.setDesktopCount(o.a);
      return true;
    case ORDER_DESKTOP_STEP: {
      const to = step(state.current, o.a);
      const active = o.b ? windows.find((w) => ws.isWindow(w) && (w.flags & F.ACTIVATED)) : null;
      if (active && active.top.vd !== null) {
        moveWindow(active, to);
      }
      setCurrent(to, active);
      if (active) {
        ws.activate(active);
      }
      return true;
    }
    case ORDER_SHOW_DESKTOP:
      setShowing(onOff(o.a, state.showing));
      return true;
    }
    return false;
  };

  // A window that maps for the first time: on the current desktop (unless a rule put it elsewhere already).
  ws.desktopsMapped = (surface) => {
    const t = surface.top;
    if (t && t.vd === undefined) {
      t.vd = state.list[state.current].id;
    }
  };

  // kwinrc again (kwin.js: reconfigure, reloadConfig).
  ws.desktopsReload = () => {
    const before = JSON.stringify([state.list, state.rows, state.current, state.wrap]);
    const oldList = state.list.slice();
    const oldRows = state.rows;
    load();
    if (JSON.stringify([state.list, state.rows, state.current, state.wrap]) !== before) {
      redraw();
      publish();
      if (ws.kwinDesktopsChanged) {
        ws.kwinDesktopsChanged("reloaded", { oldList, oldRows });
      }
    }
  };

  load();
  publish();
  log("desktops: " + state.list.length + " (" + state.list.map((d) => d.name).join(", ") + "), " + state.rows + " rows");
})
