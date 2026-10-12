// Windows: the shell (xdg_wm_base and what hangs off it), decorations, and
// what the page asks of a window through the table's orders ring.
//
// A toplevel's place is kept as where its window geometry's top left corner
// is on the screen (top.wx, top.wy); the surface's own corner follows from
// the geometry the client last set. A popup's place is a box relative to its
// parent's window geometry, worked out from its positioner by the rules of
// xdg-shell (anchor, gravity, offset; then flip, slide and resize to stay on
// the screen). So when a window moves, its popups move with it.
//
// Decorations: a window whose client agrees (zxdg_decoration_manager_v1, which
// Qt uses: we always answer server_side; org_kde_kwin_server_decoration, which
// GTK 3 uses: we take the mode it asks for) or that never asked about them
// gets a frame drawn by the page around its window geometry: a title bar of
// chrome.title pixels above it, with the buttons, and a border of
// chrome.border pixels to resize by. The page does the dragging and says what
// it wants through the orders ring (table.js: orders); a client that asks to
// be moved or resized itself (xdg_toplevel.move/resize, e.g. a Firefox with
// its own title bar) is moved by the pointer here (seat.js: grabs).
(function (ws) {
  "use strict";

  const { send, log, table, screen, chrome, H, global, windows, F } = ws;

  // xdg_toplevel's states and capabilities, and xdg_positioner's numbers
  const STATE_MAXIMIZED = 1, STATE_FULLSCREEN = 2, STATE_RESIZING = 3, STATE_ACTIVATED = 4;
  const CAP_MAXIMIZE = 2, CAP_FULLSCREEN = 3, CAP_MINIMIZE = 4;
  const EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8;
  const SLIDE_X = 1, SLIDE_Y = 2, FLIP_X = 4, FLIP_Y = 8, RESIZE_X = 16, RESIZE_Y = 32;
  // The page's orders (README.md: the orders ring)
  const ORDER_ACTIVATE = 1, ORDER_MOVE = 2, ORDER_RESIZE = 3, ORDER_RESIZE_END = 4, ORDER_CLOSE = 5,
        ORDER_MINIMIZE = 6, ORDER_MAXIMIZE = 7, ORDER_RESTORE = 8, ORDER_FULLSCREEN = 9, ORDER_LAUNCH = 10,
        ORDER_KEEP_ABOVE = 11, ORDER_KEEP_BELOW = 12, ORDER_SHADE = 13, ORDER_GEOMETRY = 17, ORDER_TILE = 18;
  // (the shell's orders, PLASMA-PROTOCOL.md: a is 1 set, 0 clear, 2 toggle)
  const onOff = (a) => (a === 2 ? null : a !== 0);
  const MIN_W = 64, MIN_H = 32;

  let placed = 0;		// where the next new window goes (a staircase)
  let tokens = 0;

  const states = (list) => {
    const a = new Uint8Array(4 * list.length);
    const v = new DataView(a.buffer);
    list.forEach((s, i) => v.setUint32(4 * i, s, true));
    return a;
  };

  // ---- the usable area: the screen less the strips the page keeps for its panels, on any edge (README.md:
  // out_panel, out_top, out_left, out_right). Maximized windows fill it, new ones are placed in it; popups and
  // fullscreen windows have the whole screen. [x, y, w, h]
  // The layer surfaces' exclusive zones (a panel of plasmashell's: layershell.js, ws.layerWork) take their strips
  // off it too, the larger of the two on each edge.
  const area = () => {
    let top = screen.top || 0, left = screen.left || 0, right = screen.right || 0, bottom = screen.panel || 0;
    const lw = ws.layerWork;
    if (lw) {
      top = Math.max(top, lw[1]);
      left = Math.max(left, lw[0]);
      right = Math.max(right, screen.width - lw[0] - lw[2]);
      bottom = Math.max(bottom, screen.height - lw[1] - lw[3]);
    }
    return [left, top, Math.max(64, screen.width - left - right), Math.max(64, screen.height - top - bottom)];
  };
  ws.workArea = area;

  // ---- window states that are KWin's rather than xdg-shell's (desktops.js has the desktops)

  // Is a window (a root surface) where it cannot be seen: minimized, or on another desktop?
  const away = (surface) => !!((surface.flags & F.MINIMIZED) || (ws.onCurrentDesktop && !ws.onCurrentDesktop(surface)));
  ws.windowAway = away;
  // Does the pointer go to this surface? Not if its window is away, nor if its window is shaded (rolled up to its
  // title bar, which the page draws: the surface is not drawn).
  ws.inputHidden = (surface) => {
    const r = ws.root(surface);
    return away(r) || !!(r.top && r.top.shaded);
  };

  // KWin's layers (kwin 6.3, effect/globals.h: Layer; Window::belongsToLayer, LayerShellV1Window::belongsToLayer),
  // bottom to top: the desktop, windows kept below (and layer surfaces of the bottom layer), the normal ones, windows
  // kept above (panels, applet popups, layer surfaces of the top layer), notifications, the active fullscreen
  // window, tooltips, critical notifications, on-screen displays, the overlay layer. Popups (menus, X tooltips) over
  // all of them.
  const L_DESKTOP = 0, L_BELOW = 1, L_NORMAL = 2, L_ABOVE = 3, L_NOTIFICATION = 4, L_ACTIVE = 5, L_POPUP = 6,
        L_CRITICAL = 7, L_OSD = 8, L_OVERLAY = 9;
  // Plasma's roles of a window (org_kde_plasma_surface.role, plasmashell.js: top.plasmaRole)
  const ROLE_DESKTOP = 1, ROLE_PANEL = 2, ROLE_OSD = 3, ROLE_NOTIFICATION = 4, ROLE_TOOLTIP = 5, ROLE_CRITICAL = 6,
        ROLE_APPLETPOPUP = 7;
  const layerOf = (surface) => {
    if (surface.layer) {
      return [L_DESKTOP, L_BELOW, L_ABOVE, L_OVERLAY][surface.layer.state.layer] ?? L_ABOVE;
    }
    const t = surface.top;
    if (!t || !isWindow(surface)) {
      return L_NORMAL;
    }
    switch (t.plasmaRole) {
    case ROLE_DESKTOP:
      return L_DESKTOP;
    case ROLE_PANEL:
    case ROLE_APPLETPOPUP:
      return L_ABOVE;
    case ROLE_TOOLTIP:
      return L_POPUP;
    case ROLE_OSD:
      return L_OSD;
    case ROLE_NOTIFICATION:
      return L_NOTIFICATION;
    case ROLE_CRITICAL:
      return L_CRITICAL;
    }
    if (t.below) {
      return L_BELOW;
    }
    if ((surface.flags & F.FULLSCREEN) && (surface.flags & F.ACTIVATED)) {
      return L_ACTIVE;
    }
    return t.above ? L_ABOVE : L_NORMAL;
  };
  // Does the window take the keyboard when activated? Not a tooltip or an on-screen display of Plasma's, nor a
  // notification unless it asked to (KWin's XdgToplevelWindow::acceptsFocus).
  const acceptsFocus = (surface) => {
    const t = surface.top;
    switch (t && t.plasmaRole) {
    case ROLE_TOOLTIP:
    case ROLE_OSD:
      return false;
    case ROLE_NOTIFICATION:
    case ROLE_CRITICAL:
      return !!t.takesFocus;
    }
    return true;
  };
  ws.acceptsFocus = acceptsFocus;
  const isPopup = (w) => (w.xdg && w.xdg.popup) || (w.xwin && !w.top);
  // The windows in their layers, keeping the order within each (and every window's subsurfaces with it).
  const stack = () => {
    const popups = windows.filter(isPopup);
    const groups = new Map();
    for (const w of windows) {
      if (isPopup(w)) {
        continue;
      }
      const r = ws.root(w);
      if (!groups.has(r)) {
        groups.set(r, []);
      }
      groups.get(r).push(w);
    }
    const roots = [...groups.keys()];
    const order = roots.map((r, i) => [layerOf(r), i, r]).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    windows.length = 0;
    for (const [, , r] of order) {
      windows.push(...groups.get(r));
    }
    windows.push(...popups);
    ws.restack();
  };
  ws.stack = stack;

  // Keep above, keep below, shade: `on` true, false, or null to toggle.
  const setKeep = (surface, which, on) => {
    const t = surface.top;
    const now = !!t[which];
    on = on === null ? !now : !!on;
    if (on === now) {
      return;
    }
    t[which] = on;
    if (on) {
      t[which === "above" ? "below" : "above"] = false;
    }
    stack();
    dirtyTree(surface);
    ws.statesChanged(surface);
  };
  ws.setKeepAbove = (surface, on) => setKeep(surface, "above", on);
  ws.setKeepBelow = (surface, on) => setKeep(surface, "below", on);
  ws.setShaded = (surface, on) => {
    const t = surface.top;
    on = on === null ? !t.shaded : !!on;
    if (on === !!t.shaded || (on && !ws.decorated(surface))) {
      return;	// (a window with no frame of ours has no title bar to roll up to)
    }
    t.shaded = on;
    dirtyTree(surface);
    ws.statesChanged(surface);
    ws.seatChanged();
  };
  // Something of a window's states changed: whoever mirrors them is told (xwm.js: _NET_WM_STATE).
  ws.statesChanged = (surface) => {
    if (surface.xwin && ws.xwmStates) {
      ws.xwmStates(surface);
    }
  };

  // ---- where things are

  const geomOrigin = (surface) => {
    const p = ws.screenPos(surface);
    const g = ws.geometry(surface);
    return [p[0] + g[0], p[1] + g[1]];
  };

  ws.shellPos = (surface) => {
    const xdg = surface.xdg;
    const g = ws.geometry(surface);
    if (xdg.popup) {
      const p = xdg.popup;
      const o = p.parent ? geomOrigin(p.parent) : [0, 0];
      return [o[0] + p.box[0] - g[0], o[1] + p.box[1] - g[1]];
    }
    if (surface.top) {
      return [surface.top.wx - g[0], surface.top.wy - g[1]];
    }
    return [surface.x, surface.y];
  };

  ws.decorated = (surface) => {
    const t = surface.top;
    if (!t || (surface.flags & F.FULLSCREEN) || t.noBorder) {
      return false;	// (noBorder: a window rule's, rules.js)
    }
    if (surface.xwin) {
      return ws.xwmDecorated(surface);	// (an X window: unless it asked for none, xwm.js)
    }
    if (t.plasmaRole) {
      return false;	// (a panel, popup, tooltip, notification, ... of Plasma's shell draws all of itself)
    }
    if (t.deco) {
      return true;		// (xdg-decoration: always server_side here)
    }
    return surface.kdeMode !== 1;	// (KDE's: unless the client asked to draw its own; nothing asked: ours)
  };

  // Everything drawn of a window (its subsurfaces, its popups) is to be written again: it moved.
  const dirtyTree = (rootSurface) => {
    for (const w of windows) {
      if (ws.root(w) === rootSurface) {
        w.dirty = true;
      }
    }
  };
  ws.dirtyTree = dirtyTree;

  // ---- configure

  // A window that the shell manages: an xdg toplevel, or an X window (xwm.js) with a frame of ours.
  const isWindow = (w) => !!(w.top && ((w.xdg && w.xdg.toplevel) || w.xwin));
  ws.isWindow = isWindow;

  const configure = (surface) => {
    if (surface.xwin) {
      ws.xwmConfigure(surface);	// (an X window is told by ConfigureWindow: xwm.js)
      return;
    }
    const xdg = surface.xdg;
    if (!xdg) {
      return;
    }
    xdg.configured = true;
    const serial = ws.nextSerial();
    xdg.lastSerial = serial;
    xdg.waitingAck = true;
    if (xdg.toplevel) {
      const t = surface.top;
      const object = xdg.toplevel;
      if (!t.sentCapabilities) {
        t.sentCapabilities = true;
        send(object, "wm_capabilities", states([CAP_MAXIMIZE, CAP_FULLSCREEN, CAP_MINIMIZE]));
      }
      const a = area();
      const bounds = [a[2] - 2 * chrome.border, a[3] - chrome.title - chrome.border];
      if (t.bounds !== bounds.join("x")) {
        t.bounds = bounds.join("x");
        send(object, "configure_bounds", bounds[0], bounds[1]);
      }
      const list = [];
      let [w, h] = t.size;
      if (surface.flags & F.FULLSCREEN) {
        list.push(STATE_FULLSCREEN);
        w = screen.width;
        h = screen.height;
      } else if (surface.flags & F.MAXIMIZED) {
        list.push(STATE_MAXIMIZED);
        w = a[2];
        h = a[3] - (ws.decorated(surface) ? chrome.title : 0);	// (not over the page's panels)
      }
      if (t.resize) {
        list.push(STATE_RESIZING);
      }
      if (surface.flags & F.ACTIVATED) {
        list.push(STATE_ACTIVATED);
      }
      if (t.deco) {
        send(t.deco, "configure", 2);	// server_side
      }
      send(object, "configure", w, h, states(list));
    } else if (xdg.popup) {
      const p = xdg.popup;
      send(p.object, "configure", p.box[0], p.box[1], p.box[2], p.box[3]);
    }
    send(xdg.object, "configure", serial);
  };

  // A new configure for a window that has one already; held back while the client has not acknowledged the
  // last (a client that is slow to draw would otherwise get one for every move of the pointer).
  const reconfigure = (surface) => {
    if (surface.xwin) {
      if (surface.top) {
        ws.xwmConfigure(surface);
        surface.dirty = true;
      }
      return;
    }
    const xdg = surface.xdg;
    if (!xdg || !xdg.configured) {
      return;
    }
    if (xdg.waitingAck) {
      xdg.again = true;
      return;
    }
    configure(surface);
    surface.dirty = true;
  };
  ws.reconfigure = reconfigure;

  // From compositor.js's commit: false if the surface is not to be shown (not configured yet, or dismissed).
  ws.shellCommit = (surface, buffer) => {
    const xdg = surface.xdg;
    if (!xdg.toplevel && !xdg.popup) {
      return false;
    }
    if (!xdg.configured) {
      if (surface.top && ws.applyRules) {
        ws.applyRules(surface);	// (before the first configure: a rule's size, maximized, ... go with it)
      }
      if (xdg.popup && !xdg.popup.parent) {
        guessPopupParent(surface);
      }
      configure(surface);
      return buffer ? true : false;
    }
    if (xdg.popup && xdg.popup.dismissed) {
      return false;
    }
    const t = surface.top;
    if (t && buffer) {
      // A window resized from its left or top edge keeps its right or bottom edge where it was.
      if (t.anchor) {
        const g = ws.geometry(surface);
        if (t.anchor.edges & EDGE_LEFT) {
          t.wx = t.anchor.right - g[2];
        }
        if (t.anchor.edges & EDGE_TOP) {
          t.wy = t.anchor.bottom - g[3];
        }
        if (t.anchorDone) {
          t.anchor = null;
          t.anchorDone = false;
        }
      }
      dirtyTree(surface);
    }
    return true;
  };

  ws.shellPlaceable = (surface) => {
    const xdg = surface.xdg;
    if (xdg.popup) {
      return !xdg.popup.dismissed && (!xdg.popup.parent || xdg.popup.parent.slot >= 0);
    }
    return !!xdg.toplevel;
  };

  // ---- a window appears, goes, is activated

  // From compositor.js: a surface was given a slot just now.
  ws.mapped = (surface) => {
    if (surface.top) {
      if (ws.applyRules) {
        ws.applyRules(surface);	// (kwinrulesrc, once: rules.js)
      }
      if (ws.desktopsMapped) {
        ws.desktopsMapped(surface);	// (on the current desktop, unless a rule said another)
      }
      place(surface);
      if (!away(surface)) {
        activate(surface);	// (a window a rule minimized or put on another desktop is not: KWin does the same)
      } else {
        stack();
      }
    } else if (surface.xdg && surface.xdg.popup) {
      dirtyTree(ws.root(surface));
    }
    if (ws.seatChanged) {
      ws.seatChanged();
    }
  };

  ws.unmapped = (surface) => {
    if (surface.top && (surface.flags & F.ACTIVATED)) {
      surface.flags &= ~F.ACTIVATED;
      activateNext(surface);
    }
    if (ws.seatChanged) {
      ws.seatChanged();
    }
  };

  const place = (surface) => {
    const t = surface.top;
    if (t.placed) {
      return;
    }
    t.placed = true;
    const g = ws.geometry(surface);
    const a = area();
    if (surface.flags & (F.MAXIMIZED | F.FULLSCREEN)) {
      t.wx = surface.flags & F.FULLSCREEN ? 0 : a[0];
      t.wy = surface.flags & F.FULLSCREEN ? 0 : a[1] + (ws.decorated(surface) ? chrome.title : 0);
      return;
    }
    const top = ws.decorated(surface) ? chrome.title + chrome.border : 0;
    const parent = t.parent && t.parent.slot >= 0 ? t.parent : null;
    if (t.plasmaPos) {
      // Where Plasma's shell said (org_kde_plasma_surface.set_position: an applet's popup, a tooltip, a
      // notification): KWin puts the window there as it is, in the screen's coordinates.
      t.wx = t.plasmaPos[0];
      t.wy = t.plasmaPos[1];
      return;
    }
    if (t.underCursor && ws.pointerAt) {
      // open_under_cursor: at the pointer, within the usable area (KWin: keepInArea).
      const [px, py] = ws.pointerAt();
      t.wx = Math.max(a[0], Math.min(Math.round(px), a[0] + a[2] - g[2]));
      t.wy = Math.max(a[1], Math.min(Math.round(py), a[1] + a[3] - g[3]));
      return;
    }
    if (parent) {
      // A dialog: over the middle of its parent.
      const o = geomOrigin(parent), pg = ws.geometry(parent);
      t.wx = Math.round(o[0] + (pg[2] - g[2]) / 2);
      t.wy = Math.round(o[1] + (pg[3] - g[3]) / 2);
    } else {
      // New windows go down a staircase from the top left corner, around again when they reach the edge.
      const step = 28;
      t.wx = a[0] + 24 + step * placed;
      t.wy = a[1] + top + 12 + step * placed;
      if (t.wx + Math.min(g[2], 500) > a[0] + a[2] || t.wy + Math.min(g[3], 300) > a[1] + a[3]) {
        placed = 0;
        t.wx = a[0] + 24;
        t.wy = a[1] + top + 12;
      }
      placed++;
    }
    // In the usable area, as far as it goes.
    t.wx = Math.max(a[0] + chrome.border, Math.min(t.wx, a[0] + a[2] - g[2] - chrome.border));
    t.wy = Math.max(a[1] + top, Math.min(t.wy, a[1] + a[3] - g[3] - chrome.border));
  };

  // The windows from the top: toplevels that can take the keyboard.
  const toplevelsFromTop = () => windows.filter((w) => isWindow(w) && !away(w) && acceptsFocus(w)).reverse();

  const activateNext = (gone) => {
    const next = toplevelsFromTop().find((w) => w !== gone);
    if (next) {
      activate(next);
    } else {
      if (ws.setKeyboardFocus) {
        ws.setKeyboardFocus(null);
      }
      if (ws.xwmFocus) {
        ws.xwmFocus(null);
      }
    }
  };

  // Make a window the active one: on top, its title bar lit, the keyboard its.
  const activate = (surface) => {
    surface = ws.root(surface);
    if (!isWindow(surface)) {
      return;
    }
    if (!acceptsFocus(surface)) {
      raise(surface);	// (a tooltip, an on-screen display: on top, but the keyboard stays where it is)
      return;
    }
    if (ws.showingDesktopEnd) {
      ws.showingDesktopEnd(surface);	// (KWin leaves "showing desktop" when a window is activated)
    }
    if (ws.onCurrentDesktop && !ws.onCurrentDesktop(surface)) {
      ws.toWindowsDesktop(surface);	// (KWin's default: the desktop the window is on becomes the current one)
    }
    if (surface.flags & F.MINIMIZED) {
      surface.flags &= ~F.MINIMIZED;
      surface.dirty = true;
    }
    for (const w of windows) {
      if (w !== surface && w.top && (w.flags & F.ACTIVATED)) {
        w.flags &= ~F.ACTIVATED;
        w.dirty = true;
        reconfigure(w);
      }
    }
    if (!(surface.flags & F.ACTIVATED)) {
      surface.flags |= F.ACTIVATED;
      surface.dirty = true;
      reconfigure(surface);
    }
    raise(surface);
    if (ws.setKeyboardFocus) {
      ws.setKeyboardFocus(surface);
    }
    if (ws.xwmFocus) {
      ws.xwmFocus(surface);	// (X's own input focus follows: to this window if it is an X one, else to none)
    }
  };
  ws.activate = activate;
  ws.activateNext = activateNext;

  // A window and everything of it (subsurfaces, popups) to the top, in the order they were in.
  const raise = (surface) => {
    const mine = windows.filter((w) => ws.root(w) === surface);
    if (!mine.length) {
      return;
    }
    const others = windows.filter((w) => ws.root(w) !== surface);
    // (Popups of other windows stay above: they are menus open somewhere. X menus and tooltips too.)
    const popups = others.filter(isPopup);
    const rest = others.filter((w) => !isPopup(w));
    windows.length = 0;
    windows.push(...rest, ...mine, ...popups);
    stack();	// (then each in its layer: a window kept above stays above the one raised)
  };
  ws.raise = raise;

  // ---- popups: where the positioner puts them

  // The box [x, y, w, h], relative to the parent's window geometry, that the positioner `p` gives inside the
  // screen, for a parent whose geometry's corner is at `origin` on the screen.
  const position = (p, origin) => {
    const area = [-origin[0], -origin[1], screen.width, screen.height];
    const box = (anchor, gravity, ox, oy) => {
      const ar = p.anchorRect;
      let ax = ar[0] + ar[2] / 2, ay = ar[1] + ar[3] / 2;
      if (anchor === 3 || anchor === 5 || anchor === 6) {
        ax = ar[0];
      } else if (anchor === 4 || anchor === 7 || anchor === 8) {
        ax = ar[0] + ar[2];
      }
      if (anchor === 1 || anchor === 5 || anchor === 7) {
        ay = ar[1];
      } else if (anchor === 2 || anchor === 6 || anchor === 8) {
        ay = ar[1] + ar[3];
      }
      const [w, h] = p.size;
      let x = ax - w / 2, y = ay - h / 2;
      if (gravity === 3 || gravity === 5 || gravity === 6) {
        x = ax - w;
      } else if (gravity === 4 || gravity === 7 || gravity === 8) {
        x = ax;
      }
      if (gravity === 1 || gravity === 5 || gravity === 7) {
        y = ay - h;
      } else if (gravity === 2 || gravity === 6 || gravity === 8) {
        y = ay;
      }
      return [Math.round(x + ox), Math.round(y + oy), w, h];
    };
    const outX = (b) => b[0] < area[0] || b[0] + b[2] > area[0] + area[2];
    const outY = (b) => b[1] < area[1] || b[1] + b[3] > area[1] + area[3];
    const flipX = { 3: 4, 4: 3, 5: 7, 7: 5, 6: 8, 8: 6 };
    const flipY = { 1: 2, 2: 1, 5: 6, 6: 5, 7: 8, 8: 7 };
    let b = box(p.anchor, p.gravity, p.offset[0], p.offset[1]);
    const c = p.constraint;
    if (outX(b) && (c & FLIP_X)) {
      const f = box(flipX[p.anchor] || p.anchor, flipX[p.gravity] || p.gravity, -p.offset[0], p.offset[1]);
      if (!outX(f)) {
        b[0] = f[0];
      }
    }
    if (outY(b) && (c & FLIP_Y)) {
      const f = box(flipY[p.anchor] || p.anchor, flipY[p.gravity] || p.gravity, p.offset[0], -p.offset[1]);
      if (!outY(f)) {
        b[1] = f[1];
      }
    }
    if (outX(b) && (c & SLIDE_X)) {
      if (b[0] + b[2] > area[0] + area[2]) {
        b[0] = area[0] + area[2] - b[2];
      }
      if (b[0] < area[0]) {
        b[0] = area[0];
      }
    }
    if (outY(b) && (c & SLIDE_Y)) {
      if (b[1] + b[3] > area[1] + area[3]) {
        b[1] = area[1] + area[3] - b[3];
      }
      if (b[1] < area[1]) {
        b[1] = area[1];
      }
    }
    if (outX(b) && (c & RESIZE_X)) {
      const x0 = Math.max(b[0], area[0]), x1 = Math.min(b[0] + b[2], area[0] + area[2]);
      if (x1 > x0) {
        b = [x0, b[1], x1 - x0, b[3]];
      }
    }
    if (outY(b) && (c & RESIZE_Y)) {
      const y0 = Math.max(b[1], area[1]), y1 = Math.min(b[1] + b[3], area[1] + area[3]);
      if (y1 > y0) {
        b = [b[0], y0, b[2], y1 - y0];
      }
    }
    return b;
  };
  ws.position = position;	// (for tests)

  const placePopup = (surface) => {
    const p = surface.xdg.popup;
    const origin = p.parent ? geomOrigin(p.parent) : [0, 0];
    p.box = position(p.positioner, origin);
    const ps = p.positioner;
    log("popup " + surface.id + ": parent " + (p.parent ? p.parent.id + (p.parent.layer ? " (layer surface)" : "") + " at " + origin.join(",")
        : "none") + ", anchor rect " + ps.anchorRect.join(",") + " anchor " + ps.anchor + " gravity " + ps.gravity + " offset "
        + ps.offset.join(",") + " size " + ps.size.join(",") + " constraint " + ps.constraint + " -> " + p.box.join(","));
  };
  ws.placePopup = placePopup;	// (layershell.js: a popup given a layer surface for its parent)

  // A popup made with no parent (xdg_surface.get_popup with a null parent) is to be given one before its first
  // commit by another protocol: zwlr_layer_surface_v1.get_popup for a panel's menus (layershell.js). The native
  // plasmashell never sends that one (Qt 6.8.2's xdg-shell makes the popup, its layer-shell's attachPopup
  // does not follow; seen 2026-10-11 with WLSERVER_DEBUG=1: get_popup(null), grab, commit), so its menus were
  // placed relative to the screen's corner (the panel's task menu at the top of the screen). Rather than the
  // protocol error, the parent is what the client most likely means, as Qt itself guesses a transient parent:
  // the surface of the same client that was pressed last (a menu opens from a click), else the one with the
  // pointer, else the keyboard, else its topmost window.
  const guessPopupParent = (surface) => {
    const client = surface.object.client;
    const ok = (s) => s && s !== surface && s.object && s.object.client === client && s.slot >= 0
      && !(s.xdg && s.xdg.popup && s.xdg.popup.dismissed);
    const candidates = [ws.lastPressSurface, ws.pointerFocus && ws.pointerFocus(), ws.keyboardFocus && ws.keyboardFocus(),
                        ...windows.filter((w) => isWindow(w) && !away(w)).reverse()];
    const parent = candidates.find(ok);
    if (!parent) {
      log("popup " + surface.id + ": no parent was given, and none of the client's surfaces fits: at the screen's corner");
      return;
    }
    surface.xdg.popup.parent = parent;
    log("popup " + surface.id + ": no parent was given: taken to be " + parent.id
        + (parent.layer ? " (a layer surface)" : parent.xdg && parent.xdg.popup ? " (a popup)" : " (a window)"));
    placePopup(surface);
  };

  // Close a popup from here (a click elsewhere): the client is told, and destroys it.
  ws.dismissPopup = (surface) => {
    const xdg = surface.xdg;
    if (!xdg || !xdg.popup || xdg.popup.dismissed) {
      return;
    }
    // Its own popups first.
    for (const w of [...ws.surfaces]) {
      if (w.xdg && w.xdg.popup && w.xdg.popup.parent === surface) {
        ws.dismissPopup(w);
      }
    }
    xdg.popup.dismissed = true;
    send(xdg.popup.object, "popup_done");
    ws.unmapSurface(surface);
  };

  // ---- the page's orders

  const byId = (id) => windows.find((w) => w.id === id && w.top);

  const minSize = (t) => [Math.max(MIN_W, t.min[0]), Math.max(MIN_H, t.min[1])];

  ws.orders = () => {
    const list = table.orders();
    if (!list) {
      return;
    }
    for (const o of list) {
      if (o.op === ORDER_LAUNCH) {
        ws.launch(o.a, o.b >>> 0, o.n);	// (a program, not a window: apps.js)
        continue;
      }
      if (o.id === 0 && ws.desktopOrder && ws.desktopOrder(o, null)) {
        continue;	// (the desktops' orders: desktops.js)
      }
      const surface = byId(o.id);
      if (!surface) {
        continue;
      }
      const t = surface.top;
      switch (o.op) {
      case ORDER_ACTIVATE:
        activate(surface);
        break;
      case ORDER_MOVE:
        moveWindow(surface, o.a, o.b);
        break;
      case ORDER_RESIZE:
        resizeWindow(surface, o.a, o.b, o.c);
        break;
      case ORDER_RESIZE_END:
        resizeEnd(surface);
        break;
      case ORDER_CLOSE:
        if (surface.xwin) {
          ws.xwmClose(surface);	// (WM_DELETE_WINDOW, or the client is let go of)
        } else {
          send(surface.xdg.toplevel, "close");
        }
        break;
      case ORDER_MINIMIZE:
        minimize(surface);
        break;
      case ORDER_MAXIMIZE:
        setMaximized(surface, !(surface.flags & F.MAXIMIZED));
        break;
      case ORDER_RESTORE:
        activate(surface);
        break;
      case ORDER_FULLSCREEN:
        setFullscreen(surface, !(surface.flags & F.FULLSCREEN));
        break;
      case ORDER_KEEP_ABOVE:
        ws.setKeepAbove(surface, onOff(o.a));
        break;
      case ORDER_KEEP_BELOW:
        ws.setKeepBelow(surface, onOff(o.a));
        break;
      case ORDER_SHADE:
        ws.setShaded(surface, onOff(o.a));
        break;
      case ORDER_GEOMETRY:
        setGeometry(surface, o.a, o.b, o.c, o.d);
        break;
      case ORDER_TILE:
        setTile(surface, o.a);
        break;
      default:
        if (ws.desktopOrder && ws.desktopOrder(o, surface)) {
          break;
        }
        log("shell: order " + o.op + " is not known");
      }
    }
  };

  // Move a window's geometry corner to (wx, wy): not off the top (its title bar stays reachable).
  const moveWindow = (surface, wx, wy) => {
    const t = surface.top;
    if (surface.flags & (F.MAXIMIZED | F.FULLSCREEN)) {
      return;
    }
    if (t.tile) {
      untile(surface, false);	// (dragged out of its tile: its size from before, where it is dragged to)
    }
    const a = area();
    t.wx = Math.max(-ws.geometry(surface)[2] + 32, Math.min(wx, screen.width - 32));
    t.wy = Math.max(a[1] + (ws.decorated(surface) ? chrome.title : 0), Math.min(wy, a[1] + a[3] - 8));
    t.anchor = null;
    dirtyTree(surface);
    ws.seatChanged();
  };
  ws.moveWindow = moveWindow;

  // Ask a window to be w x h (its geometry), dragged by `edges` (xdg_toplevel's numbers): the edges opposite
  // stay where they are. resizeEnd when the drag is over.
  const resizeWindow = (surface, edges, w, h) => {
    const t = surface.top;
    if (surface.flags & (F.MAXIMIZED | F.FULLSCREEN)) {
      return;
    }
    if (t.tile) {
      t.tile = 0;	// (resized by hand: no longer in its tile; the size from before is forgotten)
      t.tileSaved = null;
      surface.dirty = true;
    }
    if (!t.resize) {
      const g = ws.geometry(surface);
      t.resize = { edges };
      t.anchor = { edges, right: t.wx + g[2], bottom: t.wy + g[3] };
      t.anchorDone = false;
      t.anchorEnd = false;
    }
    const [mw, mh] = minSize(t);
    w = Math.max(mw, w);
    h = Math.max(mh, h);
    if (t.max[0] > 0) {
      w = Math.min(w, t.max[0]);
    }
    if (t.max[1] > 0) {
      h = Math.min(h, t.max[1]);
    }
    if (t.size[0] === w && t.size[1] === h) {
      return;
    }
    t.size = [w, h];
    reconfigure(surface);
  };
  ws.resizeWindow = resizeWindow;

  const resizeEnd = (surface) => {
    const t = surface.top;
    if (t.resize) {
      t.resize = null;
      t.anchorEnd = true;	// (the anchor goes once the client has drawn at the last size: see ack_configure)
      reconfigure(surface);
    }
  };
  ws.resizeEnd = resizeEnd;

  // The window geometry at (x, y), w x h at once (the page's quick tiling and window menu): a maximized window is
  // not any more (its size from before is not brought back), and the client is configured as for a resize with no
  // edge held.
  const setGeometry = (surface, x, y, w, h) => {
    const t = surface.top;
    if (surface.flags & F.FULLSCREEN) {
      return;
    }
    if (surface.flags & F.MAXIMIZED) {
      surface.flags &= ~F.MAXIMIZED;
      t.saved = null;
    }
    const [mw, mh] = minSize(t);
    t.size = [Math.max(mw, w | 0), Math.max(mh, h | 0)];
    if (t.max[0] > 0) {
      t.size[0] = Math.min(t.size[0], t.max[0]);
    }
    if (t.max[1] > 0) {
      t.size[1] = Math.min(t.size[1], t.max[1]);
    }
    t.wx = x | 0;
    t.wy = y | 0;
    t.anchor = null;
    dirtyTree(surface);
    reconfigure(surface);
    ws.seatChanged();
  };
  ws.setGeometry = setGeometry;

  // Quick tiling (KWin's QuickTileFlag: 1 left, 2 right, 4 top, 8 bottom, their sums for the corners, 15
  // maximize; 0 out of the tile, the size from before back): the rectangle within the usable area, the frame
  // filling it as a maximized window's fills the area.
  const tileRect = (tile) => {
    const [ax, ay, aw, ah] = area();
    let x = ax, y = ay, w = aw, h = ah;
    const hw = Math.floor(aw / 2), hh = Math.floor(ah / 2);
    if ((tile & 3) === 1) {
      w = hw;
    } else if ((tile & 3) === 2) {
      x = ax + hw;
      w = aw - hw;
    }
    if ((tile & 12) === 4) {
      h = hh;
    } else if ((tile & 12) === 8) {
      y = ay + hh;
      h = ah - hh;
    }
    return [x, y, w, h];
  };
  const placeTile = (surface) => {
    const t = surface.top;
    const [x, y, w, h] = tileRect(t.tile);
    const title = ws.decorated(surface) ? chrome.title : 0;
    t.wx = x;
    t.wy = y + title;
    t.size = [Math.max(minSize(t)[0], w), Math.max(minSize(t)[1], h - title)];
    t.anchor = null;
    dirtyTree(surface);
    reconfigure(surface);
  };
  const untile = (surface, back) => {
    const t = surface.top;
    const saved = t.tileSaved;
    t.tile = 0;
    t.tileSaved = null;
    if (saved) {
      t.size = saved.size;
      if (back) {
        t.wx = saved.wx;
        t.wy = saved.wy;
      }
      fit(surface, t.size);
    }
    dirtyTree(surface);
    reconfigure(surface);
  };
  const setTile = (surface, tile) => {
    const t = surface.top;
    tile &= 15;
    if (surface.flags & F.FULLSCREEN) {
      return;
    }
    if (tile === 15) {
      if (t.tile && t.tile !== 15) {
        untile(surface, true);
      }
      setMaximized(surface, true);
      t.tile = 15;
      surface.dirty = true;
      return;
    }
    if (t.tile === 15 || (!tile && (surface.flags & F.MAXIMIZED))) {
      t.tile = 0;
      setMaximized(surface, false);
      if (!tile) {
        return;
      }
    }
    if (!tile) {
      if (t.tile) {
        untile(surface, true);
      }
      return;
    }
    if (!t.tile) {
      const g = ws.geometry(surface);
      t.tileSaved = { wx: t.wx, wy: t.wy, size: [g[2], g[3]] };
    }
    t.tile = tile;
    placeTile(surface);
  };
  ws.setTile = setTile;

  const minimize = (surface) => {
    if (surface.flags & F.MINIMIZED) {
      return;
    }
    surface.flags |= F.MINIMIZED;
    surface.dirty = true;
    if (surface.flags & F.ACTIVATED) {
      surface.flags &= ~F.ACTIVATED;
      reconfigure(surface);
      activateNext(surface);
    }
    if (ws.seatChanged) {
      ws.seatChanged();
    }
  };

  const setMaximized = (surface, on) => {
    const t = surface.top;
    if (on === !!(surface.flags & F.MAXIMIZED)) {
      reconfigure(surface);	// (the client is to be told all the same: xdg-shell says so)
      return;
    }
    if (on) {
      const g = ws.geometry(surface);
      t.saved = { wx: t.wx, wy: t.wy, size: [g[2], g[3]] };
      surface.flags |= F.MAXIMIZED;
      t.wx = area()[0];
      t.wy = area()[1] + (ws.decorated(surface) ? chrome.title : 0);
    } else {
      surface.flags &= ~F.MAXIMIZED;
      if (t.tile === 15) {
        t.tile = 0;
      }
      if (t.saved) {
        t.wx = t.saved.wx;
        t.wy = t.saved.wy;
        t.size = t.saved.size;
        t.saved = null;
        fit(surface, t.size);	// (the screen may be smaller now than when it was maximized)
      }
    }
    t.anchor = null;
    dirtyTree(surface);
    reconfigure(surface);
  };

  const setFullscreen = (surface, on) => {
    const t = surface.top;
    if (on === !!(surface.flags & F.FULLSCREEN)) {
      reconfigure(surface);
      return;
    }
    if (on) {
      const g = ws.geometry(surface);
      t.savedFull = { wx: t.wx, wy: t.wy, size: [g[2], g[3]] };
      surface.flags |= F.FULLSCREEN;
      t.wx = 0;
      t.wy = 0;
    } else {
      surface.flags &= ~F.FULLSCREEN;
      if (t.savedFull) {
        t.wx = t.savedFull.wx;
        t.wy = t.savedFull.wy;
        t.size = (surface.flags & F.MAXIMIZED) ? t.size : t.savedFull.size;
        t.savedFull = null;
        if (!(surface.flags & F.MAXIMIZED)) {
          fit(surface, t.size);
        }
      }
      if (surface.flags & F.MAXIMIZED) {
        t.wx = area()[0];
        t.wy = area()[1] + (ws.decorated(surface) ? chrome.title : 0);
      }
    }
    t.anchor = null;
    stack();	// (the active fullscreen window is a layer of its own)
    dirtyTree(surface);
    reconfigure(surface);
  };

  ws.minimize = minimize;
  ws.setMaximized = setMaximized;
  ws.setFullscreen = setFullscreen;

  // ---- the screen changed size (compositor.js: setOutput)

  // Windows are fitted to the new screen: maximized and fullscreen ones take all of it, others stay on it (their
  // title bars where they can be reached), and a window bigger than the screen is asked to be smaller.
  // A window of geometry size `size` ([w, h]) put on the screen: smaller if it does not fit (t.size: the client
  // is asked for that with the next configure), and moved so that all of it, or its title bar, is on the screen.
  const fit = (surface, size) => {
    const t = surface.top;
    const top = ws.decorated(surface) ? chrome.title + chrome.border : 0;
    const a = area();
    const maxW = a[2] - 2 * chrome.border, maxH = a[3] - top - chrome.border;
    let [w, h] = size;
    if (w > maxW || h > maxH) {
      const [mw, mh] = minSize(t);
      w = Math.max(mw, Math.min(w, maxW));
      h = Math.max(mh, Math.min(h, maxH));
      t.size = [w, h];
    }
    t.wx = Math.max(a[0] + chrome.border, Math.min(t.wx, a[0] + a[2] - Math.min(w, maxW) - chrome.border));
    t.wy = Math.max(a[1] + top, Math.min(t.wy, a[1] + a[3] - Math.min(h, maxH) - chrome.border));
  };

  ws.outputChanged = () => {
    for (const surface of windows) {
      const t = surface.top;
      if (!isWindow(surface)) {
        continue;
      }
      if (t.tile && t.tile !== 15 && !(surface.flags & (F.MAXIMIZED | F.FULLSCREEN))) {
        placeTile(surface);	// (its tile in the new area)
      } else if (!(surface.flags & (F.MAXIMIZED | F.FULLSCREEN))) {
        const g = ws.geometry(surface);
        fit(surface, [g[2], g[3]]);
        t.anchor = null;
      } else if (!(surface.flags & F.FULLSCREEN)) {
        t.wx = area()[0];	// (the panels' strips may have changed)
        t.wy = area()[1] + (ws.decorated(surface) ? chrome.title : 0);
      }
      reconfigure(surface);	// (the new bounds; the new size, if there is one, or all of the screen)
      dirtyTree(surface);
    }
    if (ws.seatChanged) {
      ws.seatChanged();
    }
    if (ws.workAreaChanged) {
      ws.workAreaChanged();
    }
  };

  // ---- the handlers

  H.xdg_wm_base = {
    destroy() {},
    create_positioner(base, id) {
      const object = ws.create(base.client, id, "xdg_positioner", base.version);
      object.positioner = { size: [0, 0], anchorRect: [0, 0, 0, 0], anchor: 0, gravity: 0, constraint: 0, offset: [0, 0] };
    },
    get_xdg_surface(base, id, surfaceObject) {
      const surface = surfaceObject.surface;
      if (surface.role && surface.role !== "xdg") {
        ws.error(base.client, base, 1, "get_xdg_surface: the surface has a role already");
        return;
      }
      const object = ws.create(base.client, id, "xdg_surface", base.version);
      object.surface = surface;
      surface.role = "xdg";
      surface.xdg = { object, toplevel: null, popup: null, configured: false, lastSerial: 0, waitingAck: false, again: false };
      object.onDestroy = () => {
        ws.unmapSurface(surface);
        surface.xdg = null;
      };
    },
    pong() {},
  };

  H.xdg_positioner = {
    destroy() {},
    set_size(p, w, h) {
      p.positioner.size = [w, h];
    },
    set_anchor_rect(p, x, y, w, h) {
      p.positioner.anchorRect = [x, y, w, h];
    },
    set_anchor(p, a) {
      p.positioner.anchor = a;
    },
    set_gravity(p, g) {
      p.positioner.gravity = g;
    },
    set_constraint_adjustment(p, c) {
      p.positioner.constraint = c;
    },
    set_offset(p, x, y) {
      p.positioner.offset = [x, y];
    },
    set_reactive() {},
    set_parent_size() {},
    set_parent_configure() {},
  };

  const copyPositioner = (p) => ({ size: p.size.slice(), anchorRect: p.anchorRect.slice(), anchor: p.anchor,
                                   gravity: p.gravity, constraint: p.constraint, offset: p.offset.slice() });

  H.xdg_surface = {
    destroy() {},
    get_toplevel(xdgObject, id) {
      const surface = xdgObject.surface;
      const object = ws.create(xdgObject.client, id, "xdg_toplevel", xdgObject.version);
      object.surface = surface;
      surface.xdg.toplevel = object;
      surface.top = {
        wx: 0, wy: 0, size: [0, 0], min: [0, 0], max: [0, 0], resize: null, anchor: null, anchorDone: false,
        anchorEnd: false, deco: null, parent: null, saved: null, savedFull: null, placed: false, sentCapabilities: false,
      };
      object.onDestroy = () => {
        ws.unmapSurface(surface);
        if (surface.top && surface.top.deco) {
          surface.top.deco.surface = null;
        }
        surface.top = null;
        surface.flags &= ~(F.ACTIVATED | F.MAXIMIZED | F.FULLSCREEN | F.MINIMIZED);
        if (surface.xdg) {
          surface.xdg.toplevel = null;
          surface.xdg.configured = false;
        }
        // Dialogs of this window lose their parent.
        for (const w of ws.surfaces) {
          if (w.top && w.top.parent === surface) {
            w.top.parent = null;
          }
        }
      };
    },
    get_popup(xdgObject, id, parentObject, positionerObject) {
      const surface = xdgObject.surface;
      const object = ws.create(xdgObject.client, id, "xdg_popup", xdgObject.version);
      object.surface = surface;
      const parent = parentObject ? parentObject.surface : null;
      surface.xdg.popup = { object, parent, positioner: copyPositioner(positionerObject.positioner), box: [0, 0, 0, 0],
                            grab: false, dismissed: false };
      placePopup(surface);
      object.onDestroy = () => {
        ws.unmapSurface(surface);
        if (ws.popupGone) {
          ws.popupGone(surface);
        }
        if (surface.xdg) {
          surface.xdg.popup = null;
          surface.xdg.configured = false;
        }
      };
    },
    set_window_geometry(xdgObject, x, y, w, h) {
      const surface = xdgObject.surface;
      surface.geometry = [x, y, w, h];
      surface.dirty = true;
    },
    ack_configure(xdgObject, serial) {
      const surface = xdgObject.surface;
      const xdg = surface.xdg;
      if (!xdg) {
        return;
      }
      if (serial === xdg.lastSerial) {
        xdg.waitingAck = false;
        if (xdg.again) {
          xdg.again = false;
          configure(surface);
          surface.dirty = true;
        } else if (surface.top && surface.top.anchorEnd && !surface.top.resize) {
          surface.top.anchorEnd = false;
          surface.top.anchorDone = true;	// (the next commit is at the final size: the anchor is used once more)
        }
      }
    },
  };

  H.xdg_toplevel = {
    destroy() {},
    set_parent(t, parent) {
      t.surface.top.parent = parent ? parent.surface : null;
    },
    set_title(t, title) {
      t.surface.title = title || "";
      t.surface.dirty = true;
    },
    set_app_id(t, appId) {
      t.surface.appId = appId || "";
      t.surface.dirty = true;
    },
    show_window_menu() {},
    move(t) {
      if (ws.startGrab) {
        ws.startGrab(t.surface, "move", 0);
      }
    },
    resize(t, seat, serial, edges) {
      if (ws.startGrab) {
        ws.startGrab(t.surface, "resize", edges);
      }
    },
    set_max_size(t, w, h) {
      t.surface.top.max = [w, h];
    },
    set_min_size(t, w, h) {
      t.surface.top.min = [w, h];
    },
    set_maximized(t) {
      setMaximized(t.surface, true);
    },
    unset_maximized(t) {
      setMaximized(t.surface, false);
    },
    set_fullscreen(t) {
      setFullscreen(t.surface, true);
    },
    unset_fullscreen(t) {
      setFullscreen(t.surface, false);
    },
    set_minimized(t) {
      minimize(t.surface);
    },
  };

  H.xdg_popup = {
    destroy() {},
    grab(popup, seat, serial) {
      popup.surface.xdg.popup.grab = true;
      if (ws.popupGrab) {
        ws.popupGrab(popup.surface);
      }
    },
    reposition(popup, positionerObject, token) {
      const surface = popup.surface;
      surface.xdg.popup.positioner = copyPositioner(positionerObject.positioner);
      placePopup(surface);
      send(popup, "repositioned", token);
      configure(surface);
      surface.dirty = true;
    },
  };

  // Decorations, the xdg way (Qt): server_side, always.
  H.zxdg_decoration_manager_v1 = {
    destroy() {},
    get_toplevel_decoration(manager, id, toplevel) {
      const object = ws.create(manager.client, id, "zxdg_toplevel_decoration_v1", 1);
      const surface = toplevel.surface;
      object.surface = surface;
      if (surface.top) {
        surface.top.deco = object;
      }
      object.onDestroy = () => {
        if (object.surface && object.surface.top && object.surface.top.deco === object) {
          object.surface.top.deco = null;
        }
      };
    },
  };

  H.zxdg_toplevel_decoration_v1 = {
    destroy() {},
    set_mode(object) {
      if (object.surface) {
        reconfigure(object.surface);
      }
    },
    unset_mode(object) {
      if (object.surface) {
        reconfigure(object.surface);
      }
    },
  };

  // Decorations, KDE's way (GTK 3): the mode the client asks for.
  H.org_kde_kwin_server_decoration_manager = {
    create(manager, id, surfaceObject) {
      const object = ws.create(manager.client, id, "org_kde_kwin_server_decoration", 1);
      const surface = surfaceObject.surface;
      object.surface = surface;
      surface.kdeMode = 2;
      send(object, "mode", 2);
      object.onDestroy = () => {
        surface.kdeMode = 0;
        surface.dirty = true;
      };
    },
  };

  H.org_kde_kwin_server_decoration = {
    release() {},
    request_mode(object, mode) {
      object.surface.kdeMode = mode;
      object.surface.dirty = true;
      send(object, "mode", mode);
    },
  };

  // Activation (a program asking for one of its windows to come forward): granted.
  H.xdg_activation_v1 = {
    destroy() {},
    get_activation_token(activation, id) {
      ws.create(activation.client, id, "xdg_activation_token_v1", 1);
    },
    activate(activation, token, surfaceObject) {
      const surface = ws.root(surfaceObject.surface);
      if (surface.top && surface.slot >= 0) {
        activate(surface);
      }
    },
  };

  H.xdg_activation_token_v1 = {
    set_serial() {},
    set_app_id() {},
    set_surface() {},
    commit(token) {
      send(token, "done", "rvwasm-" + ++tokens);
    },
    destroy() {},
  };

  global("xdg_wm_base", 6, () => {});
  global("zxdg_decoration_manager_v1", 1, () => {});
  global("org_kde_kwin_server_decoration_manager", 1, (manager) => {
    send(manager, "default_mode", 2);
  });
  global("xdg_activation_v1", 1, () => {});
})
