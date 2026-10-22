// Layer surfaces: zwlr_layer_shell_v1 (wlr-layer-shell-unstable-v1, version 5, the one layer-shell-qt 6.3
// asks for), which is how plasmashell puts up its desktop (the background layer, all of the screen) and its
// panels (the top layer, anchored to an edge, with an exclusive zone that windows are kept out of). Done as KWin
// 6.3 does it (src/wayland/layershell_v1.cpp, src/layershellv1window.cpp, src/layershellv1integration.cpp):
//
// - A layer surface's state (size, anchor, exclusive zone and edge, margins, keyboard interactivity, layer) is
//   double-buffered: it counts from the commit after the requests. Its first commit (no buffer) is answered by a
//   configure with the size it gets; a buffer before that configure is a protocol error.
// - Where the surfaces go: every committed surface is arranged on the screen in KWin's order: those with an
//   exclusive zone first, the overlay layer down to the background, each within the area the ones before have
//   left (an exclusive zone of -1: within the whole screen); then the others, the same way. A surface anchored to
//   two opposite edges with no size asked for in that direction gets all of the area between them. What the
//   exclusive zones leave is the usable area (ws.layerWork), which shell.js's area() takes in: maximized and new
//   windows are placed within it, as they are within the page's own panel strips.
// - Layers in the stacking order (shell.js: layerOf): background under everything, bottom with the windows kept
//   below, top with the windows kept above, overlay over all but popups.
// - Keyboard: a surface whose keyboard interactivity is not none takes the keyboard when it maps (the desktop's
//   only when no window has it), when it asks for it later (top and overlay layers), and when it is clicked;
//   the window that had it is deactivated, as KWin's activateWindow does. When the surface lets go of the
//   keyboard (unmapped, interactivity none) the topmost window gets it back.
// - xdg popups of a layer surface (zwlr_layer_surface_v1.get_popup: a panel's context menu) are placed relative
//   to it, as a window's are.
// - kde_screen_edge_manager_v1 (kde-screen-edge-v1, plasmashell's auto-hiding panels): an auto hide edge,
//   activated, hides its surface (not drawn, no input) until the pointer touches that edge of the screen, or the
//   client deactivates it.
(function (ws) {
  "use strict";

  const { send, log, screen, H, global, windows, F } = ws;

  const BACKGROUND = 0, BOTTOM = 1, TOP = 2, OVERLAY = 3;
  const A_TOP = 1, A_BOTTOM = 2, A_LEFT = 4, A_RIGHT = 8;
  const KI_NONE = 0, KI_ON_DEMAND = 2;
  // zwlr_layer_shell_v1.error, zwlr_layer_surface_v1.error
  const E_ROLE = 0, E_INVALID_LAYER = 1, E_ALREADY_CONSTRUCTED = 2;
  const E_SURFACE_STATE = 0, E_SIZE = 1, E_ANCHOR = 2, E_KEYBOARD = 3, E_EDGE = 4;
  // kde_screen_edge_manager_v1: its error and border numbers
  const E_INVALID_BORDER = 0, E_INVALID_ROLE = 1, E_EDGE_CONSTRUCTED = 2;
  const B_TOP = 1, B_BOTTOM = 2, B_LEFT = 3, B_RIGHT = 4;

  const layers = [];	// surfaces that have a layer surface, oldest first (KWin arranges them in this order)
  let focused = null;	// the layer surface that has the keyboard

  const freshState = (layer) => ({ size: [0, 0], anchor: 0, zone: 0, edge: 0, margin: [0, 0, 0, 0], kbd: KI_NONE, layer });

  // The edge an exclusive zone is on (KWin's LayerSurfaceV1Interface::exclusiveEdge): the one set, or the one
  // edge the surface is anchored to (alone, or with both of its neighbours); none without a zone.
  const exclusiveEdge = (st) => {
    if (st.zone <= 0) {
      return 0;
    }
    if (st.edge) {
      return st.edge;
    }
    const a = st.anchor;
    if (a === (A_LEFT | A_TOP | A_RIGHT) || a === A_TOP) {
      return A_TOP;
    }
    if (a === (A_TOP | A_RIGHT | A_BOTTOM) || a === A_RIGHT) {
      return A_RIGHT;
    }
    if (a === (A_RIGHT | A_BOTTOM | A_LEFT) || a === A_BOTTOM) {
      return A_BOTTOM;
    }
    if (a === (A_BOTTOM | A_LEFT | A_TOP) || a === A_LEFT) {
      return A_LEFT;
    }
    return 0;
  };

  // ---- where they go

  const sameRect = (a, b) => !!a && !!b && a[0] === b[0] && a[1] === b[1] && a[2] === b[2] && a[3] === b[3];

  // One surface's rectangle within `bounds` ([x, y, w, h]), by KWin's rearrangeLayer.
  const place = (st, bounds) => {
    let [x, y] = [0, 0];
    let [w, h] = st.size;
    const [mt, mr, mb, ml] = st.margin;
    const horizontal = (st.anchor & (A_LEFT | A_RIGHT)) === (A_LEFT | A_RIGHT);
    const vertical = (st.anchor & (A_TOP | A_BOTTOM)) === (A_TOP | A_BOTTOM);
    if ((st.anchor & (A_LEFT | A_RIGHT)) && w === 0) {
      x = bounds[0];
      w = bounds[2];
    } else if (st.anchor & A_LEFT) {
      x = bounds[0];
    } else if (st.anchor & A_RIGHT) {
      x = bounds[0] + bounds[2] - w;
    } else {
      x = bounds[0] + Math.trunc((bounds[2] - w) / 2);
    }
    if ((st.anchor & (A_TOP | A_BOTTOM)) && h === 0) {
      y = bounds[1];
      h = bounds[3];
    } else if (st.anchor & A_TOP) {
      y = bounds[1];
    } else if (st.anchor & A_BOTTOM) {
      y = bounds[1] + bounds[3] - h;
    } else {
      y = bounds[1] + Math.trunc((bounds[3] - h) / 2);
    }
    if (horizontal) {
      x += ml;
      w -= ml + mr;
    } else if (st.anchor & A_LEFT) {
      x += ml;
    } else if (st.anchor & A_RIGHT) {
      x -= mr;
    }
    if (vertical) {
      y += mt;
      h -= mt + mb;
    } else if (st.anchor & A_TOP) {
      y += mt;
    } else if (st.anchor & A_BOTTOM) {
      y -= mb;
    }
    return [x, y, w, h];
  };

  // The area less a surface's exclusive zone (KWin's adjustWorkArea).
  const reserve = (st, work) => {
    const [mt, mr, mb, ml] = st.margin;
    switch (exclusiveEdge(st)) {
    case A_LEFT:
      return [work[0] + ml + st.zone, work[1], work[2] - ml - st.zone, work[3]];
    case A_RIGHT:
      return [work[0], work[1], work[2] - mr - st.zone, work[3]];
    case A_TOP:
      return [work[0], work[1] + mt + st.zone, work[2], work[3] - mt - st.zone];
    case A_BOTTOM:
      return [work[0], work[1], work[2], work[3] - mb - st.zone];
    }
    return work;
  };

  const configure = (surface, w, h) => {
    const L = surface.layer;
    const serial = ws.nextSerial();
    L.serials.push(serial);
    L.sent = [w, h];
    L.configured = true;
    send(L.object, "configure", serial, w, h);
  };

  const close = (surface) => {
    const L = surface.layer;
    if (!L.closed) {
      L.closed = true;
      send(L.object, "closed");
      log("layershell: closed " + (L.namespace || "a surface") + " (no room for it on the screen)");
    }
    ws.unmapSurface(surface);
    letGo(surface);
  };

  // Every layer surface to its place, configured with its size; the usable area worked out again (and the windows
  // fitted to it, when it changed).
  const arrange = () => {
    const full = [0, 0, screen.width, screen.height];
    let work = full;
    const list = layers.filter((s) => s.layer && s.layer.committed && !s.layer.closed);
    for (const exclusive of [true, false]) {
      for (const layer of [OVERLAY, TOP, BOTTOM, BACKGROUND]) {
        for (const s of list) {
          const L = s.layer, st = L.state;
          if (st.layer !== layer || exclusive !== (st.zone > 0) || L.closed) {
            continue;
          }
          const g = place(st, st.zone === -1 ? full : work);
          if (g[2] <= 0 || g[3] <= 0) {
            close(s);
            continue;
          }
          if (!sameRect(g, L.geom)) {
            L.geom = g;
            s.x = g[0];
            s.y = g[1];
            ws.dirtyTree(s);	// (its popups move with it)
          }
          if (!L.sent || L.sent[0] !== g[2] || L.sent[1] !== g[3]) {
            configure(s, g[2], g[3]);
            log("layershell: " + (L.namespace || "a surface") + " (layer " + st.layer + ", anchors " + st.anchor
                + (st.zone ? ", exclusive zone " + st.zone : "") + ") at " + g.join(","));
          }
          if (exclusive) {
            work = reserve(st, work);
          }
        }
      }
    }
    const before = ws.layerWork;
    ws.layerWork = sameRect(work, full) ? null : work;
    if (!sameRect(before || full, ws.layerWork || full)) {
      log("layershell: the usable area is " + (ws.layerWork || full).join(",") + " (the layer surfaces' exclusive zones)");
      shellOutputChanged();	// (maximized windows refitted, _NET_WORKAREA, ...)
    }
    ws.seatChanged();
  };
  ws.layerArrange = arrange;

  // The part of a layer surface that is the panel. plasmashell's surface is larger than its panel: a floating panel has a gap of
  // transparent pixels around it (and room for its shadow), and the surface keeps its size when the panel is flush with the screen's
  // edge. What plasmashell says is the panel is its input region (it masks the rest, so that a click in the gap goes to the window
  // under it): the box of the region's added rectangles within the surface, [x, y, w, h] in surface pixels; null when there is
  // no region, or none that is inside the surface (the surface is all of it). The page cuts windows' frames only by this, so a
  // title bar next to the transparent gap is not eaten by it (display.js holesAbove).
  ws.layerVisible = (surface) => {
    if (!surface.layer || !surface.input) {
      return null;
    }
    const [w, h] = ws.size(surface);
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const r of surface.input) {
      if (r[0] === 1) {
        x0 = Math.min(x0, r[1]);
        y0 = Math.min(y0, r[2]);
        x1 = Math.max(x1, r[1] + r[3]);
        y1 = Math.max(y1, r[2] + r[4]);
      }
    }
    x0 = Math.max(0, x0);
    y0 = Math.max(0, y0);
    x1 = Math.min(w, x1);
    y1 = Math.min(h, y1);
    const box = x1 > x0 && y1 > y0 ? [x0, y0, x1 - x0, y1 - y0] : null;
    const said = box ? box.join(",") : "";
    if (surface.layer.saidVisible !== said) {
      surface.layer.saidVisible = said;
      log("layershell: " + (surface.layer.namespace || "a surface") + " (" + w + "x" + h + ") has its input region at " + (said || "(none inside it)"));
    }
    return box;
  };

  // The screen changed: the layer surfaces are arranged on it before the windows are fitted to what they leave.
  const shellOutputChanged = ws.outputChanged;
  ws.outputChanged = () => {
    const before = ws.layerWork;
    arrange();
    if (sameRect(before || [], ws.layerWork || [])) {
      shellOutputChanged();
    }
  };

  // ---- the keyboard

  const wantsKeyboard = (surface) => surface.layer && surface.layer.state.kbd !== KI_NONE && !surface.layer.hidden;

  // The layer surface takes the keyboard; the active window is not active any more (KWin's activateWindow).
  const takeFocus = (surface) => {
    if (focused === surface) {
      return;
    }
    for (const w of windows) {
      if (w.top && (w.flags & F.ACTIVATED)) {
        w.flags &= ~F.ACTIVATED;
        w.dirty = true;
        ws.reconfigure(w);
      }
    }
    ws.setKeyboardFocus(surface);
    if (ws.xwmFocus) {
      ws.xwmFocus(null);
    }
  };

  // The surface does not want the keyboard any more, or is gone: the topmost window has it again.
  const letGo = (surface) => {
    if (focused === surface) {
      focused = null;
      ws.activateNext(null);
    }
  };

  // Whoever gets the keyboard, we know whether it is a layer surface of ours.
  const setKeyboardFocus = ws.setKeyboardFocus;
  ws.setKeyboardFocus = (surface) => {
    focused = surface && surface.layer ? surface : null;
    setKeyboardFocus(surface);
  };

  // A press on a layer surface (seat.js): one that takes the keyboard on demand takes it.
  ws.layerPressed = (surface) => {
    if (wantsKeyboard(surface)) {
      takeFocus(surface);
    }
  };

  // A layer surface was given a slot (compositor.js, then shell.js's mapped): it takes the keyboard if it wants
  // it (the desktop only when no window is active: KWin's Workspace::addWaylandWindow).
  const shellMapped = ws.mapped;
  ws.mapped = (surface) => {
    shellMapped(surface);
    if (!surface.layer) {
      return;
    }
    if (wantsKeyboard(surface)) {
      const desktop = surface.layer.state.layer === BACKGROUND || surface.layer.namespace === "desktop";
      if (!desktop || !windows.some((w) => w.top && (w.flags & F.ACTIVATED))) {
        takeFocus(surface);
      }
    }
    ws.stack();
  };

  const shellUnmapped = ws.unmapped;
  ws.unmapped = (surface) => {
    shellUnmapped(surface);
    if (surface.layer) {
      letGo(surface);
    }
  };

  // ---- commits (compositor.js): false if the surface is not to be shown

  ws.layerCommit = (surface, buffer) => {
    const L = surface.layer;
    if (L.closed) {
      return false;
    }
    const object = L.object;
    if (buffer && !L.configured) {
      ws.error(object.client, object, E_SURFACE_STATE,
               "a buffer has been attached to a layer surface prior to the first layer_surface.configure event");
      return false;
    }
    const p = L.pending;
    L.pending = {};
    const st = L.state;
    const anchor = p.anchor !== undefined ? p.anchor : st.anchor;
    if (p.size && p.size[0] === 0 && (anchor & (A_LEFT | A_RIGHT)) !== (A_LEFT | A_RIGHT)) {
      ws.error(object.client, object, E_SIZE,
               "the layer surface has a width of 0 but its anchor doesn't include the left and the right screen edge");
      return false;
    }
    if (p.size && p.size[1] === 0 && (anchor & (A_TOP | A_BOTTOM)) !== (A_TOP | A_BOTTOM)) {
      ws.error(object.client, object, E_SIZE,
               "the layer surface has a height of 0 but its anchor doesn't include the top and the bottom screen edge");
      return false;
    }
    if (p.edge !== undefined || p.anchor !== undefined) {
      const edge = p.edge !== undefined ? p.edge : st.edge;
      if (edge && !(edge & anchor)) {
        ws.error(object.client, object, E_EDGE, "Exclusive edge is not of the anchors");
        return false;
      }
    }
    // A commit with no buffer after it had one: unmapped, back to what it was when it was made (the spec; the
    // client commits again for a new configure).
    if (!buffer && L.mappedOnce) {
      L.state = freshState(L.created);
      L.committed = false;
      L.mappedOnce = false;
      L.configured = false;
      L.serials = [];
      L.sent = null;
      L.geom = null;
      ws.unmapSurface(surface);
      letGo(surface);
      arrange();
      return false;
    }
    const before = JSON.stringify([st.size, st.anchor, st.zone, st.edge, st.margin, st.layer]);
    const kbdBefore = st.kbd;
    for (const key of Object.keys(p)) {
      st[key] = p[key];
    }
    const first = !L.committed;
    L.committed = true;
    if (buffer) {
      L.mappedOnce = true;
    }
    if (first || JSON.stringify([st.size, st.anchor, st.zone, st.edge, st.margin, st.layer]) !== before) {
      if (!first && p.layer !== undefined) {
        ws.stack();
      }
      arrange();
    }
    if (L.closed) {
      return false;
    }
    // Keyboard interactivity changed (KWin's handleAcceptsFocusChanged: the top and overlay layers take the
    // keyboard when they ask for it; letting it go gives it back to the windows).
    if (st.kbd !== kbdBefore && surface.slot >= 0) {
      if (st.kbd === KI_NONE) {
        letGo(surface);
      } else if (st.layer === TOP || st.layer === OVERLAY) {
        takeFocus(surface);
      }
    }
    return !!buffer;
  };

  ws.layerPlaceable = (surface) => surface.layer.configured && !surface.layer.closed;

  // The role the page is told of (the slot's role word, as org_kde_plasma_surface's roles: 1 desktop, 2 panel, 3
  // on-screen display, 4 notification, 5 tooltip, 6 critical notification), from the namespace (KWin's
  // scopeToType).
  const roles = { desktop: 1, dock: 2, "on-screen-display": 3, notification: 4, tooltip: 5, "crititical-notification": 6,
                  "critical-notification": 6 };
  ws.layerRole = (surface) => roles[(surface.layer.namespace || "").toLowerCase()] || 0;

  // ---- the requests

  H.zwlr_layer_shell_v1 = {
    get_layer_surface(shell, id, surfaceObject, output, layer, namespace) {
      const surface = surfaceObject.surface;
      if (surface.current.buffer) {
        ws.error(shell.client, shell, E_ALREADY_CONSTRUCTED, "the wl_surface already has a buffer attached");
        return;
      }
      if (layer > OVERLAY) {
        ws.error(shell.client, shell, E_INVALID_LAYER, "invalid layer " + layer);
        return;
      }
      if (surface.role && surface.role !== "layer") {
        ws.error(shell.client, shell, E_ROLE, "the wl_surface already has a role assigned " + surface.role);
        return;
      }
      const object = ws.create(shell.client, id, "zwlr_layer_surface_v1", shell.version);
      object.surface = surface;
      surface.role = "layer";
      surface.layer = {
        object, namespace: namespace || "", created: layer, state: freshState(layer), pending: {}, configured: false,
        committed: false, mappedOnce: false, closed: false, serials: [], sent: null, geom: null, hidden: false, edge: null,
      };
      layers.push(surface);
      object.onDestroy = () => {
        ws.unmapSurface(surface);
        const at = layers.indexOf(surface);
        if (at >= 0) {
          layers.splice(at, 1);
        }
        if (surface.layer && surface.layer.edge) {
          surface.layer.edge.surface = null;
        }
        surface.layer = null;	// (the wl_surface keeps its role, as in KWin: it cannot become anything else)
        letGo(surface);
        arrange();
      };
      if (ws.debug) {
        log("layershell: " + (namespace || "a surface") + " in layer " + layer);
      }
    },
    destroy() {},
  };

  H.zwlr_layer_surface_v1 = {
    set_size(object, w, h) {
      object.surface.layer.pending.size = [w, h];
    },
    set_anchor(object, anchor) {
      if (anchor > (A_TOP | A_BOTTOM | A_LEFT | A_RIGHT)) {
        ws.error(object.client, object, E_ANCHOR, "invalid anchor " + anchor);
        return;
      }
      object.surface.layer.pending.anchor = anchor;
    },
    set_exclusive_zone(object, zone) {
      object.surface.layer.pending.zone = zone;
    },
    set_margin(object, top, right, bottom, left) {
      object.surface.layer.pending.margin = [top, right, bottom, left];
    },
    set_keyboard_interactivity(object, k) {
      if (k > KI_ON_DEMAND || (k === KI_ON_DEMAND && object.version < 4)) {
        ws.error(object.client, object, E_KEYBOARD, "invalid keyboard interactivity " + k);
        return;
      }
      object.surface.layer.pending.kbd = k;
    },
    get_popup(object, popupObject) {
      const popupSurface = popupObject.surface;
      const xdg = popupSurface && popupSurface.xdg;
      if (!xdg || !xdg.popup) {
        return;
      }
      if (xdg.configured) {
        ws.error(object.client, object, E_SURFACE_STATE, "xdg_popup surface is already configured");
        return;
      }
      xdg.popup.parent = object.surface;
      ws.placePopup(popupSurface);
    },
    ack_configure(object, serial) {
      const L = object.surface.layer;
      if (!L.serials.includes(serial)) {
        ws.error(object.client, object, E_SURFACE_STATE, "invalid configure serial " + serial);
        return;
      }
      while (L.serials.length && L.serials.shift() !== serial) {
        // (the older ones are acknowledged with it)
      }
    },
    destroy() {},
    set_layer(object, layer) {
      if (layer > OVERLAY) {
        ws.error(object.client, object, E_INVALID_LAYER, "invalid layer " + layer);
        return;
      }
      object.surface.layer.pending.layer = layer;
    },
    set_exclusive_edge(object, edge) {
      if (edge !== 0 && edge !== A_TOP && edge !== A_BOTTOM && edge !== A_LEFT && edge !== A_RIGHT) {
        ws.error(object.client, object, E_EDGE, "Invalid exclusive edge: " + edge);
        return;
      }
      object.surface.layer.pending.edge = edge;
    },
  };

  // xdg_activation_v1.activate of a layer surface (layer-shell-qt's requestActivate): it takes the keyboard if it
  // takes it at all.
  const activationActivate = H.xdg_activation_v1.activate;
  H.xdg_activation_v1.activate = (activation, token, surfaceObject) => {
    const surface = ws.root(surfaceObject.surface);
    if (surface.layer) {
      if (surface.slot >= 0 && wantsKeyboard(surface)) {
        takeFocus(surface);
      }
      return;
    }
    activationActivate(activation, token, surfaceObject);
  };

  // ---- auto-hiding panels: kde_screen_edge_manager_v1

  const setHidden = (surface, hidden) => {
    const L = surface.layer;
    if (!L || L.hidden === hidden) {
      return;
    }
    L.hidden = hidden;
    if (hidden) {
      surface.flags |= F.MINIMIZED;	// (not drawn, no input: shell.js's away)
      letGo(surface);
    } else {
      surface.flags &= ~F.MINIMIZED;
    }
    ws.dirtyTree(surface);
    ws.seatChanged();
  };

  H.kde_screen_edge_manager_v1 = {
    destroy() {},
    get_auto_hide_screen_edge(manager, id, border, surfaceObject) {
      const surface = surfaceObject.surface;
      if (border < B_TOP || border > B_RIGHT) {
        ws.error(manager.client, manager, E_INVALID_BORDER, "invalid border " + border);
        return;
      }
      if (!surface.layer) {
        ws.error(manager.client, manager, E_INVALID_ROLE, "surface has invalid role");
        return;
      }
      if (surface.layer.edge) {
        ws.error(manager.client, manager, E_EDGE_CONSTRUCTED, "the surface already has a screen edge");
        return;
      }
      const object = ws.create(manager.client, id, "kde_auto_hide_screen_edge_v1", manager.version);
      object.surface = surface;
      object.border = border;
      object.active = false;
      surface.layer.edge = object;
      object.onDestroy = () => {
        if (object.surface && object.surface.layer) {
          setHidden(object.surface, false);
          object.surface.layer.edge = null;
        }
      };
    },
  };

  H.kde_auto_hide_screen_edge_v1 = {
    destroy() {},
    activate(object) {
      object.active = true;
      if (object.surface) {
        setHidden(object.surface, true);
      }
    },
    deactivate(object) {
      object.active = false;
      if (object.surface) {
        setHidden(object.surface, false);
      }
    },
  };

  // The pointer moved (seat.js): at a screen edge an active auto hide edge waits on, the surface comes back and
  // the edge is spent (the client activates it again when it wants to hide).
  ws.edgeTouched = (px, py) => {
    for (const s of layers) {
      const e = s.layer && s.layer.edge;
      if (!e || !e.active) {
        continue;
      }
      const at = e.border === B_TOP ? py <= 0 : e.border === B_BOTTOM ? py >= screen.height - 1
        : e.border === B_LEFT ? px <= 0 : px >= screen.width - 1;
      if (at) {
        e.active = false;
        setHidden(s, false);
      }
    }
  };

  global("zwlr_layer_shell_v1", 5, () => {});
  global("kde_screen_edge_manager_v1", 1, () => {});

  ws.layerSurfaces = layers;
})
