// Plasma's own Wayland protocols (plasma-wayland-protocols 1.16, as KWin 6.3 offers them), so that plasmashell
// can be the desktop's shell on this server: its desktop and panels are layer surfaces (layershell.js); what is
// here is the rest of what it binds.
//
// - org_kde_plasma_shell 8 (plasma-shell.xml): a window's role in the shell (desktop, panel, on-screen display,
//   notification, tooltip, critical notification, applet popup), its position (double-buffered, in the screen's
//   coordinates: where an applet's popup or a tooltip goes), open under the cursor, skip taskbar and switcher,
//   panel behaviour and takes-focus. As KWin's XdgSurfaceWindow::installPlasmaShellSurface: a window with a
//   position goes there, a role other than normal puts it on all desktops, in its layer (shell.js: layerOf),
//   without our frame, and tooltips and on-screen displays never take the keyboard (shell.js: acceptsFocus).
// - org_kde_plasma_window_management 18 (plasma-window-management.xml): the task manager's and the pager's view
//   of the windows, every xdg toplevel and X window of ours: title, app id, state flags, themed icon name (from
//   the program's .desktop file, as KWin finds it; "wayland" when there is none), pid, virtual desktops, parent,
//   geometry (the frame's, and the client's), the stacking order; and their requests (activate, minimize,
//   maximize, fullscreen, keep above and below, shade, close, move, resize, to and from a desktop, show the
//   desktop). Kept as a copy that is compared with the windows once a turn, and the differences sent.
// - org_kde_plasma_activation_feedback 1: a launch with an xdg activation token for an app id is announced, and
//   finished when a window of that app id appears (or after 20 s).
// - org_kde_plasma_virtual_desktop_management 2 (org-kde-plasma-virtual-desktop.xml): desktops.js's desktops,
//   their names, the current one, the rows; activate, create and remove. Compared once a turn too.
// - kde_output_order_v1: the one output there is ("PAGE-1"), so that plasmashell's ScreenPool needs no fallback.
// - What KWin's effects and Plasma's dialogs ask for and nothing is drawn by here: org_kde_kwin_blur_manager,
//   org_kde_kwin_contrast_manager, org_kde_kwin_slide_manager, org_kde_kwin_shadow_manager (Plasma's dialogs
//   draw their shadows in their own buffers too), org_kde_kwin_appmenu_manager,
//   org_kde_kwin_server_decoration_palette_manager, zwp_idle_inhibit_manager_v1 (the screen never sleeps here):
//   taken and forgotten. org_kde_kwin_keystate: the lock keys' states. zxdg_exporter_v2 and zxdg_importer_v2: a
//   window of one program made the parent of another's (a dialog over a window of another process).
(function (ws) {
  "use strict";

  const { send, log, screen, chrome, H, global, windows, F, sys1, sys4, sys5, SYS, alloc, dv, path, P0 } = ws;

  // org_kde_plasma_surface.role
  const ROLE_NORMAL = 0, ROLE_PANEL = 2, ROLE_TOOLTIP = 5, ROLE_APPLETPOPUP = 7;
  // org_kde_plasma_window_management.state
  const S_ACTIVE = 0x1, S_MINIMIZED = 0x2, S_MAXIMIZED = 0x4, S_FULLSCREEN = 0x8, S_KEEP_ABOVE = 0x10,
        S_KEEP_BELOW = 0x20, S_ON_ALL_DESKTOPS = 0x40, S_DEMANDS_ATTENTION = 0x80, S_CLOSEABLE = 0x100,
        S_MINIMIZABLE = 0x200, S_MAXIMIZABLE = 0x400, S_FULLSCREENABLE = 0x800, S_SKIPTASKBAR = 0x1000,
        S_SHADEABLE = 0x2000, S_SHADED = 0x4000, S_MOVABLE = 0x8000, S_RESIZABLE = 0x10000,
        S_VIRTUAL_DESKTOP_CHANGEABLE = 0x20000, S_SKIPSWITCHER = 0x40000;
  const SERVER_IDS = 0xff000000;

  const serverId = (client) => {
    client.nextServerId = client.nextServerId ? client.nextServerId + 1 : SERVER_IDS;
    return client.nextServerId;
  };

  const uuid = () => {
    const b = new Uint8Array(16);
    crypto.getRandomValues(b);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    const h = [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
    // (QUuid::toString's form, which is what KWin sends)
    return "{" + h.slice(0, 8) + "-" + h.slice(8, 12) + "-" + h.slice(12, 16) + "-" + h.slice(16, 20) + "-" + h.slice(20) + "}";
  };

  // Long titles are cut, as KWin does (a message has to fit libwayland's buffer).
  const truncate = (s) => {
    const bytes = new TextEncoder().encode(s || "");
    if (bytes.length <= 1024) {
      return s || "";
    }
    return new TextDecoder().decode(bytes.subarray(0, 1024)).replace(/�$/, "");
  };

  // ---- a client's process: its pid (SO_PEERCRED) and its program's name (KWin's resourceName for a Wayland
  // window: the executable's file name)

  const SOL_SOCKET = 1, SO_PEERCRED = 17;
  const cred = alloc(16);
  const credLen = alloc(8);
  const linkBuf = alloc(512);
  const pidOf = (client) => {
    if (client.pid !== undefined) {
      return client.pid;
    }
    client.pid = 0;
    const v = dv();
    v.setUint32(credLen, 12, true);
    if (sys5(SYS.getsockopt, client.fd, SOL_SOCKET, SO_PEERCRED, cred, credLen) === 0) {
      client.pid = dv().getInt32(cred, true) >>> 0;
    }
    return client.pid;
  };
  const programOf = (pid) => {
    if (!pid) {
      return "";
    }
    const n = sys4(SYS.readlinkat, -100, path("/proc/" + pid + "/exe", P0), linkBuf, 511);
    if (n <= 0) {
      return "";
    }
    const name = new TextDecoder().decode(ws.u8().slice(linkBuf, linkBuf + n));
    return name.slice(name.lastIndexOf("/") + 1);
  };

  // ---- a program's icon: the Icon of its .desktop file, found by its app id (KWin's iconFromDesktopFile)

  const APP_DIRS = ["/root/.local/share/applications", "/usr/local/share/applications", "/usr/share/applications"];
  const icons = new Map();
  const iconOf = (appId, x11) => {
    const fallback = x11 ? "xorg" : "wayland";
    if (!appId) {
      return fallback;
    }
    if (!icons.has(appId)) {
      let icon = "";
      for (const name of [appId, appId.toLowerCase()]) {
        for (const dir of APP_DIRS) {
          const text = /^[A-Za-z0-9._-]+$/.test(name) ? ws.readText(dir + "/" + name + ".desktop") : null;
          const m = text && /^\s*Icon\s*=\s*(.+?)\s*$/m.exec(text);
          if (m) {
            icon = m[1];
            break;
          }
        }
        if (icon) {
          break;
        }
      }
      icons.set(appId, icon);
    }
    return icons.get(appId) || fallback;
  };

  // ---- org_kde_plasma_shell

  // What a plasma surface says, applied to its window (the xdg toplevel may come before or after it).
  const applyPlasma = (surface) => {
    const p = surface.plasma, t = surface.top;
    if (!p || !t) {
      return;
    }
    const roleChanged = t.plasmaRole !== p.role;
    t.plasmaRole = p.role;
    t.takesFocus = p.takesFocus;
    t.underCursor = p.underCursor;
    t.skipTaskbar = p.skipTaskbar;
    t.skipSwitcher = p.skipSwitcher;
    if (roleChanged) {
      if (p.role !== ROLE_NORMAL) {
        t.vd = null;	// (KWin: on all desktops, all activities)
      }
      ws.stack();
      ws.dirtyTree(surface);
    }
  };

  H.org_kde_plasma_shell = {
    get_surface(shell, id, surfaceObject) {
      const surface = surfaceObject.surface;
      if (surface.plasma) {
        ws.error(shell.client, shell, 0, "org_kde_plasma_shell_surface already exists");
        return;
      }
      const object = ws.create(shell.client, id, "org_kde_plasma_surface", shell.version);
      object.surface = surface;
      surface.plasma = { object, role: ROLE_NORMAL, pos: null, pendingPos: null, behavior: 1, skipTaskbar: false,
                         skipSwitcher: false, takesFocus: false, underCursor: false };
      object.onDestroy = () => {
        if (surface.plasma === object.surface.plasma) {
          surface.plasma = null;
        }
      };
    },
  };

  const plasmaOf = (object) => object.surface && object.surface.plasma && object.surface.plasma.object === object
    ? object.surface.plasma : null;

  H.org_kde_plasma_surface = {
    destroy() {},
    set_output() {},	// (one output)
    set_position(object, x, y) {
      const p = plasmaOf(object);
      if (p) {
        p.pendingPos = [x, y];	// (from the next commit: KWin's PlasmaShellSurfaceCommit)
      }
    },
    set_role(object, role) {
      const p = plasmaOf(object);
      if (p) {
        p.role = role <= ROLE_APPLETPOPUP ? role : ROLE_NORMAL;
        applyPlasma(object.surface);
      }
    },
    set_panel_behavior(object, behavior) {
      const p = plasmaOf(object);
      if (p) {
        p.behavior = behavior >= 1 && behavior <= 4 ? behavior : 1;
      }
    },
    set_skip_taskbar(object, skip) {
      const p = plasmaOf(object);
      if (p) {
        p.skipTaskbar = !!skip;
        applyPlasma(object.surface);
      }
    },
    set_skip_switcher(object, skip) {
      const p = plasmaOf(object);
      if (p) {
        p.skipSwitcher = !!skip;
        applyPlasma(object.surface);
      }
    },
    panel_auto_hide_hide(object) {
      const p = plasmaOf(object);
      if (p && p.behavior !== 2) {
        ws.error(object.client, object, 0, "Not an auto hide panel");	// (panel_not_auto_hide)
        return;
      }
      if (p && object.surface.top && ws.minimize) {
        send(object, "auto_hidden_panel_hidden");
      }
    },
    panel_auto_hide_show(object) {
      const p = plasmaOf(object);
      if (p && p.behavior !== 2) {
        ws.error(object.client, object, 0, "Not an auto hide panel");
        return;
      }
      if (p) {
        send(object, "auto_hidden_panel_shown");
      }
    },
    set_panel_takes_focus(object, takes) {
      const p = plasmaOf(object);
      if (p) {
        p.takesFocus = !!takes;
        applyPlasma(object.surface);
        if (p.takesFocus && object.surface.top && object.surface.slot >= 0) {
          ws.activate(object.surface);
        }
      }
    },
    open_under_cursor(object) {
      const p = plasmaOf(object);
      if (!p) {
        return;
      }
      if (object.surface.current.buffer) {
        ws.error(object.client, object, 0, "open_under_cursor: surface has a buffer");
        return;
      }
      p.underCursor = true;
      applyPlasma(object.surface);
    },
  };

  // A commit of an xdg surface (compositor.js, before shell.js has it): the plasma surface's position counts from
  // now, and goes to its window (KWin moves it there unless it is being moved or resized by hand).
  const shellCommit = ws.shellCommit;
  ws.shellCommit = (surface, buffer) => {
    const p = surface.plasma;
    if (p && surface.top) {
      applyPlasma(surface);
      if (p.pendingPos) {
        const t = surface.top;
        const pos = p.pendingPos;
        p.pendingPos = null;
        if (!p.pos || p.pos[0] !== pos[0] || p.pos[1] !== pos[1]) {
          p.pos = pos;
          t.plasmaPos = pos;
          if (t.placed && !t.resize) {
            t.wx = pos[0];
            t.wy = pos[1];
            ws.dirtyTree(surface);
            ws.seatChanged();
          }
        }
      }
    }
    return shellCommit(surface, buffer);
  };

  // ---- org_kde_plasma_window_management

  const managers = new Set();	// org_kde_plasma_window_management objects
  const records = new Map();	// window (root surface) -> { uuid, id, objects: Set, last }
  let windowIds = 0;
  let lastOrder = "", lastShowing = false;

  const isManaged = (w) => ws.isWindow(w) && w.slot >= 0;

  // A window's frame (our title bar and border around its geometry) and its geometry, on the screen.
  const geometries = (w) => {
    const p = ws.screenPos(w), g = ws.geometry(w);
    const client = [p[0] + g[0], p[1] + g[1], g[2], g[3]];
    if (!ws.decorated(w)) {
      return [client, client];
    }
    const title = chrome.title;
    const b = w.flags & F.MAXIMIZED ? 0 : chrome.border;	// (no border around a maximized window: Breeze's default)
    const shaded = w.top && w.top.shaded;
    return [[client[0] - b, client[1] - title - b, client[2] + 2 * b, (shaded ? 0 : client[3]) + title + 2 * b], client];
  };

  // What the protocol says of a window now.
  const snapshot = (w) => {
    const t = w.top;
    const role = t.plasmaRole || 0;
    const special = role !== ROLE_NORMAL;
    const d = ws.windowDesktop ? ws.windowDesktop(w) : 0;	// (0-based; -1 on all)
    const list = ws.desktops ? ws.desktops.list : [];
    const fixed = t.max && t.min && t.max[0] > 0 && t.max[0] === t.min[0] && t.max[1] > 0 && t.max[1] === t.min[1];
    const resizable = !special && !fixed;
    let state = S_CLOSEABLE | S_VIRTUAL_DESKTOP_CHANGEABLE;
    if (w.flags & F.ACTIVATED) state |= S_ACTIVE;
    if (w.flags & F.MINIMIZED) state |= S_MINIMIZED;
    if (w.flags & F.MAXIMIZED) state |= S_MAXIMIZED;
    if (w.flags & F.FULLSCREEN) state |= S_FULLSCREEN;
    if (t.above) state |= S_KEEP_ABOVE;
    if (t.below) state |= S_KEEP_BELOW;
    if (d < 0) state |= S_ON_ALL_DESKTOPS;
    if (t.attention) state |= S_DEMANDS_ATTENTION;
    if (!special || (t.parent && role === ROLE_NORMAL)) state |= S_MINIMIZABLE;
    if (resizable && role !== ROLE_APPLETPOPUP) state |= S_MAXIMIZABLE | S_FULLSCREENABLE;
    if (t.skipTaskbar) state |= S_SKIPTASKBAR;
    if (t.skipSwitcher) state |= S_SKIPSWITCHER;
    if (ws.decorated(w)) state |= S_SHADEABLE;
    if (t.shaded) state |= S_SHADED;
    if (!special && !(w.flags & F.FULLSCREEN)) state |= S_MOVABLE;
    if (resizable && !(w.flags & F.FULLSCREEN)) state |= S_RESIZABLE;
    const client = w.object.client;
    const pid = w.xwin ? (w.xwin.pid || 0) : pidOf(client);
    const [frame, clientGeometry] = geometries(w);
    return {
      title: truncate(w.title), appId: truncate(w.appId), state, icon: iconOf(w.appId, !!w.xwin), pid,
      resourceName: w.xwin ? (w.xwin.inst || "") : programOf(pid),
      desktops: d < 0 || !list[d] ? [] : [list[d].id],
      parent: t.parent && t.parent.top ? t.parent : null,
      frame, client: clientGeometry,
    };
  };

  const sendPlasmaWindowParent = (object, rec) => {
    const parent = rec.last.parent ? records.get(rec.last.parent) : null;
    let target = null;
    if (parent) {
      for (const o of parent.objects) {
        if (o.manager === object.manager) {
          target = o;
        }
      }
    }
    send(object, "parent_window", target);
  };

  // KWin's PlasmaWindowInterfacePrivate::sendInitialState.
  const sendInitialState = (object, rec) => {
    const s = rec.last;
    for (const id of s.desktops) {
      send(object, "virtual_desktop_entered", id);
    }
    if (s.appId) {
      send(object, "app_id_changed", s.appId);
    }
    if (s.pid) {
      send(object, "pid_changed", s.pid);
    }
    if (s.title) {
      send(object, "title_changed", s.title);
    }
    send(object, "state_changed", s.state);
    if (s.icon) {
      send(object, "themed_icon_name_changed", s.icon);
    }
    sendPlasmaWindowParent(object, rec);
    send(object, "geometry", ...s.frame);
    send(object, "initial_state");
    if (s.resourceName) {
      send(object, "resource_name_changed", s.resourceName);
    }
    send(object, "client_geometry", ...s.client);
  };

  // A window the protocol has not heard of: announced to every manager.
  const announce = (w) => {
    const rec = { uuid: uuid(), id: ++windowIds, objects: new Set(), last: snapshot(w), surface: w };
    records.set(w, rec);
    for (const m of managers) {
      if (m.version >= 13) {
        send(m, "window_with_uuid", rec.id, rec.uuid);
      } else {
        send(m, "window", rec.id);
      }
    }
    return rec;
  };

  const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

  // Once a turn: the windows compared with what the clients were told.
  const syncWindows = () => {
    const now = windows.filter(isManaged);
    const present = new Set(now);
    for (const [w, rec] of records) {
      if (!present.has(w)) {
        records.delete(w);
        rec.gone = true;
        for (const o of rec.objects) {
          send(o, "unmapped");
        }
      }
    }
    for (const w of now) {
      const rec = records.get(w);
      if (!rec) {
        announce(w);
        feedbackSeen(w.appId);
        continue;
      }
      const s = snapshot(w), l = rec.last;
      rec.last = s;
      if (!rec.objects.size) {
        continue;
      }
      for (const o of rec.objects) {
        if (s.title !== l.title) send(o, "title_changed", s.title);
        if (s.appId !== l.appId) send(o, "app_id_changed", s.appId);
        if (s.pid !== l.pid) send(o, "pid_changed", s.pid);
        if (s.state !== l.state) send(o, "state_changed", s.state);
        if (s.icon !== l.icon) send(o, "themed_icon_name_changed", s.icon);
        if (s.resourceName !== l.resourceName) send(o, "resource_name_changed", s.resourceName);
        if (!same(s.frame, l.frame)) send(o, "geometry", ...s.frame);
        if (!same(s.client, l.client)) send(o, "client_geometry", ...s.client);
        if (s.parent !== l.parent) sendPlasmaWindowParent(o, rec);
        for (const id of l.desktops) {
          if (!s.desktops.includes(id)) send(o, "virtual_desktop_left", id);
        }
        for (const id of s.desktops) {
          if (!l.desktops.includes(id)) send(o, "virtual_desktop_entered", id);
        }
      }
    }
    // The stacking order, bottom to top.
    const order = now.map((w) => records.get(w).uuid);
    const key = order.join(";");
    if (key !== lastOrder) {
      lastOrder = key;
      for (const m of managers) {
        sendStackingOrder(m, order);
      }
    }
    const showing = !!(ws.desktops && ws.desktops.showing);
    if (showing !== lastShowing) {
      lastShowing = showing;
      for (const m of managers) {
        send(m, "show_desktop_changed", showing ? 1 : 0);
      }
    }
  };

  const sendStackingOrder = (m, order) => {
    if (m.version >= 17) {
      send(m, "stacking_order_changed_2");
    } else if (m.version >= 12) {
      send(m, "stacking_order_uuid_changed", order.join(";"));
    } else if (m.version >= 11) {
      const a = new Uint8Array(4 * order.length);
      const v = new DataView(a.buffer);
      order.forEach((u, i) => {
        const rec = [...records.values()].find((r) => r.uuid === u);
        v.setUint32(4 * i, rec ? rec.id : 0, true);
      });
      send(m, "stacking_order_changed", a);
    }
  };

  const newPlasmaWindow = (manager, id, rec) => {
    const object = ws.create(manager.client, id, "org_kde_plasma_window", manager.version);
    object.manager = manager;
    if (!rec) {
      // (KWin: a window just for this object, gone at once)
      send(object, "state_changed", 0);
      send(object, "parent_window", null);
      send(object, "initial_state");
      send(object, "unmapped");
      return;
    }
    object.rec = rec;
    rec.objects.add(object);
    object.onDestroy = () => rec.objects.delete(object);
    sendInitialState(object, rec);
  };

  H.org_kde_plasma_window_management = {
    show_desktop(manager, state) {
      if (ws.setShowingDesktop) {
        ws.setShowingDesktop(state === 1);
      }
    },
    get_window(manager, id, internalId) {
      syncWindows();
      newPlasmaWindow(manager, id, [...records.values()].find((r) => r.id === internalId) || null);
    },
    get_window_by_uuid(manager, id, wanted) {
      syncWindows();
      newPlasmaWindow(manager, id, [...records.values()].find((r) => r.uuid === wanted) || null);
    },
    get_stacking_order(manager, id) {
      const object = ws.create(manager.client, id, "org_kde_plasma_stacking_order", manager.version);
      for (const w of windows.filter(isManaged)) {
        const rec = records.get(w);
        if (rec) {
          send(object, "window", rec.uuid);
        }
      }
      send(object, "done");
      ws.destroy(object);	// (done is its destructor)
    },
  };

  // The window an org_kde_plasma_window stands for, if it is still there.
  const windowOf = (object) => object.rec && !object.rec.gone && object.rec.surface.top ? object.rec.surface : null;

  const close = (w) => {
    if (w.xwin) {
      ws.xwmClose(w);
    } else if (w.xdg && w.xdg.toplevel) {
      send(w.xdg.toplevel, "close");
    }
  };

  H.org_kde_plasma_window = {
    set_state(object, flags, state) {
      const w = windowOf(object);
      if (!w) {
        return;
      }
      log("plasmashell: " + (w.title || w.appId || "a window") + ": set_state 0x" + flags.toString(16) + " 0x" + state.toString(16));
      const on = (bit) => !!(state & bit);
      if (flags & S_MINIMIZED) {
        if (on(S_MINIMIZED)) {
          ws.minimize(w);
        } else if (w.flags & F.MINIMIZED) {
          ws.activate(w);	// (unminimized: KWin brings it back through the focus chain, to the front)
        }
      }
      if (flags & S_ACTIVE && on(S_ACTIVE)) {
        ws.activate(w);
      }
      if (flags & S_MAXIMIZED) {
        ws.setMaximized(w, on(S_MAXIMIZED));
      }
      if (flags & S_FULLSCREEN) {
        ws.setFullscreen(w, on(S_FULLSCREEN));
      }
      if (flags & S_KEEP_ABOVE) {
        ws.setKeepAbove(w, on(S_KEEP_ABOVE));
      }
      if (flags & S_KEEP_BELOW) {
        ws.setKeepBelow(w, on(S_KEEP_BELOW));
      }
      if (flags & S_SHADED) {
        ws.setShaded(w, on(S_SHADED));
      }
      if (flags & S_DEMANDS_ATTENTION) {
        w.top.attention = on(S_DEMANDS_ATTENTION);
        w.dirty = true;
        ws.statesChanged(w);
      }
      if (flags & S_SKIPTASKBAR) {
        w.top.skipTaskbar = on(S_SKIPTASKBAR);
        w.dirty = true;
      }
      if (flags & S_SKIPSWITCHER) {
        w.top.skipSwitcher = on(S_SKIPSWITCHER);
        w.dirty = true;
      }
    },
    set_virtual_desktop() {},	// (KWin leaves it blank: deprecated)
    set_minimized_geometry() {},
    unset_minimized_geometry() {},
    highlight() {},
    close(object) {
      const w = windowOf(object);
      if (w) {
        close(w);
      }
    },
    request_move(object) {
      const w = windowOf(object);
      if (w && ws.startGrab && !(w.flags & (F.MAXIMIZED | F.FULLSCREEN))) {
        ws.startGrab(w, "move", 0);
      }
    },
    request_resize(object) {
      const w = windowOf(object);
      if (w && ws.startGrab && !(w.flags & (F.MAXIMIZED | F.FULLSCREEN))) {
        ws.startGrab(w, "resize", 10);	// (by the bottom right corner, as KWin does)
      }
    },
    destroy() {},
    get_icon(object, fd) {
      // The icon as Qt's QDataStream of a QIcon is not made here: the themed icon name is always sent, and
      // libtaskmanager takes that. An empty answer.
      sys1(SYS.close, fd);
    },
    request_enter_virtual_desktop(object, id) {
      const w = windowOf(object);
      const i = ws.desktops ? ws.desktops.list.findIndex((d) => d.id === id) : -1;
      if (w && i >= 0) {
        ws.windowToDesktop(w, i);
      }
    },
    request_enter_new_virtual_desktop(object) {
      const w = windowOf(object);
      if (w && ws.createDesktop) {
        const d = ws.createDesktop(ws.desktops.list.length);
        if (d) {
          ws.windowToDesktop(w, ws.desktops.list.length - 1);
        }
      }
    },
    request_leave_virtual_desktop(object, id) {
      // A window is on one desktop or on all of them here: leaving the one it is on puts it on all (KWin: a
      // window on no desktop is on all of them); leaving one of "all" puts it on the current one.
      const w = windowOf(object);
      if (!w || !ws.desktops) {
        return;
      }
      const d = ws.windowDesktop(w);
      const list = ws.desktops.list;
      if (d < 0) {
        const i = list.findIndex((x) => x.id !== id);
        if (i >= 0 && list.length > 1) {
          ws.windowToDesktop(w, list[ws.desktops.current].id !== id ? ws.desktops.current : i);
        }
      } else if (list[d] && list[d].id === id) {
        ws.windowToDesktop(w, -1);
      }
    },
    request_enter_activity() {},
    request_leave_activity() {},
    send_to_output() {},	// (one output)
  };

  // ---- org_kde_plasma_activation_feedback: launches by app id, until a window of theirs shows

  const feedbacks = new Set();
  const activations = [];	// { appId, objects: [], at }

  const feedbackStart = (appId) => {
    if (!appId || !feedbacks.size) {
      return;
    }
    const a = { appId, objects: [], at: performance.now() };
    for (const f of feedbacks) {
      const object = ws.create(f.client, serverId(f.client), "org_kde_plasma_activation", 1);
      send(f, "activation", object);
      send(object, "app_id", appId);
      a.objects.push(object);
    }
    activations.push(a);
  };
  const feedbackDone = (a) => {
    for (const o of a.objects) {
      if (o.client.objects.get(o.id) === o) {
        send(o, "finished");
      }
    }
    activations.splice(activations.indexOf(a), 1);
  };
  function feedbackSeen(appId) {
    for (const a of activations.slice()) {
      if (appId && a.appId.toLowerCase() === appId.toLowerCase()) {
        feedbackDone(a);
      }
    }
  }
  const feedbackTurn = () => {
    const t = performance.now();
    for (const a of activations.slice()) {
      if (t - a.at > 20000) {
        feedbackDone(a);
      }
    }
  };

  H.org_kde_plasma_activation_feedback = {
    destroy() {},
  };
  H.org_kde_plasma_activation = {
    destroy() {},
  };

  // A token asked for with an app id (a launcher starting a program, kio's ApplicationLauncherJob): its launch.
  const tokenAppId = H.xdg_activation_token_v1.set_app_id;
  H.xdg_activation_token_v1.set_app_id = (token, appId) => {
    token.appId = appId;
    tokenAppId(token, appId);
  };
  const tokenCommit = H.xdg_activation_token_v1.commit;
  H.xdg_activation_token_v1.commit = (token) => {
    tokenCommit(token);
    feedbackStart(token.appId);
  };

  // ---- org_kde_plasma_virtual_desktop_management

  const vdManagers = new Set();
  const vdObjects = new Set();	// org_kde_plasma_virtual_desktop objects
  let vdLast = null;	// { list: [{ id, name }], current: id, rows }

  const vdNow = () => {
    const st = ws.desktops;
    if (!st) {
      return { list: [{ id: "desktop-1", name: "Desktop 1" }], current: "desktop-1", rows: 1 };
    }
    return { list: st.list.map((d) => ({ id: d.id, name: d.name })), current: st.list[st.current].id, rows: st.rows };
  };

  const vdDescribe = (object) => {
    const d = vdLast.list.find((x) => x.id === object.desktopId);
    if (!d) {
      send(object, "removed");
      return;
    }
    send(object, "desktop_id", d.id);
    send(object, "name", truncate(d.name));
    if (d.id === vdLast.current) {
      send(object, "activated");
    }
    send(object, "done");
  };

  const syncDesktops = () => {
    const now = vdNow();
    if (!vdLast) {
      vdLast = now;
      return;
    }
    if (JSON.stringify(now) === JSON.stringify(vdLast)) {
      return;
    }
    const before = vdLast;
    vdLast = now;
    const oldIds = before.list.map((d) => d.id), newIds = now.list.map((d) => d.id);
    let managerChanged = false;
    for (const id of oldIds) {
      if (!newIds.includes(id)) {
        managerChanged = true;
        for (const m of vdManagers) {
          send(m, "desktop_removed", id);
        }
        for (const o of vdObjects) {
          if (o.desktopId === id) {
            send(o, "removed");
            o.desktopId = null;
          }
        }
      }
    }
    now.list.forEach((d, i) => {
      if (!oldIds.includes(d.id)) {
        managerChanged = true;
        for (const m of vdManagers) {
          send(m, "desktop_created", d.id, i);
        }
      }
    });
    if (now.rows !== before.rows) {
      managerChanged = true;
      for (const m of vdManagers) {
        send(m, "rows", now.rows);
      }
    }
    for (const o of vdObjects) {
      if (!o.desktopId) {
        continue;
      }
      const d = now.list.find((x) => x.id === o.desktopId), old = before.list.find((x) => x.id === o.desktopId);
      let changed = false;
      if (old && d.name !== old.name) {
        send(o, "name", truncate(d.name));
        changed = true;
      }
      if (before.current === o.desktopId && now.current !== o.desktopId) {
        send(o, "deactivated");
        changed = true;
      }
      if (now.current === o.desktopId && before.current !== o.desktopId) {
        send(o, "activated");
        changed = true;
      }
      if (changed) {
        send(o, "done");
      }
    }
    if (managerChanged) {
      for (const m of vdManagers) {
        send(m, "done");
      }
    }
  };

  H.org_kde_plasma_virtual_desktop_management = {
    get_virtual_desktop(manager, id, desktopId) {
      syncDesktops();
      const object = ws.create(manager.client, id, "org_kde_plasma_virtual_desktop", 1);
      object.desktopId = desktopId;
      vdObjects.add(object);
      object.onDestroy = () => vdObjects.delete(object);
      vdDescribe(object);
    },
    request_create_virtual_desktop(manager, name, position) {
      if (ws.createDesktop) {
        ws.createDesktop(Math.min(position, ws.desktops.list.length), name);
      }
    },
    request_remove_virtual_desktop(manager, desktopId) {
      if (ws.removeDesktop) {
        ws.removeDesktop(desktopId);
      }
    },
  };

  H.org_kde_plasma_virtual_desktop = {
    request_activate(object) {
      const i = ws.desktops ? ws.desktops.list.findIndex((d) => d.id === object.desktopId) : -1;
      if (i >= 0) {
        ws.setCurrentDesktop(i);
      }
    },
  };

  // ---- the turn's look (after compositor.js's pump, which has written the windows' slots)

  const pump = ws.pump;
  ws.pump = () => {
    pump();
    if (managers.size || records.size) {
      syncWindows();
    }
    if (vdManagers.size || vdObjects.size) {
      syncDesktops();
    }
    if (activations.length) {
      feedbackTurn();
    }
  };

  // ---- kde_output_order_v1

  H.kde_output_order_v1 = {
    destroy() {},
  };

  // ---- what is taken and forgotten (KWin's effects; the screen never sleeps here)

  const nothing = (iface, requests, creates = {}) => {
    const h = {};
    for (const r of requests) {
      h[r] = () => {};
    }
    for (const [r, child] of Object.entries(creates)) {
      h[r] = (object, id) => {
        ws.create(object.client, id, child, object.version);
      };
    }
    H[iface] = h;
  };
  nothing("org_kde_kwin_blur_manager", ["unset"], { create: "org_kde_kwin_blur" });
  nothing("org_kde_kwin_blur", ["commit", "set_region", "release"]);
  nothing("org_kde_kwin_contrast_manager", ["unset"], { create: "org_kde_kwin_contrast" });
  nothing("org_kde_kwin_contrast", ["commit", "set_region", "set_contrast", "set_intensity", "set_saturation", "release",
                                    "set_frost"]);
  nothing("org_kde_kwin_slide_manager", ["unset"], { create: "org_kde_kwin_slide" });
  nothing("org_kde_kwin_slide", ["commit", "set_location", "set_offset", "release"]);
  nothing("org_kde_kwin_shadow_manager", ["unset", "destroy"], { create: "org_kde_kwin_shadow" });
  nothing("org_kde_kwin_shadow", ["commit", "attach_left", "attach_top_left", "attach_top", "attach_top_right",
                                  "attach_right", "attach_bottom_right", "attach_bottom", "attach_bottom_left",
                                  "set_left_offset", "set_top_offset", "set_right_offset", "set_bottom_offset", "destroy"]);
  nothing("org_kde_kwin_appmenu_manager", ["release"], { create: "org_kde_kwin_appmenu" });
  nothing("org_kde_kwin_appmenu", ["set_address", "release"]);
  nothing("org_kde_kwin_server_decoration_palette_manager", [], { create: "org_kde_kwin_server_decoration_palette" });
  nothing("org_kde_kwin_server_decoration_palette", ["set_palette", "release"]);
  nothing("zwp_idle_inhibit_manager_v1", ["destroy"], { create_inhibitor: "zwp_idle_inhibitor_v1" });
  nothing("zwp_idle_inhibitor_v1", ["destroy"]);

  // org_kde_kwin_keystate: the lock keys (keyboard.js keeps the modifiers' state: Lock is Caps Lock, Mod2 Num Lock).
  const LOCK = 2, MOD2 = 16;
  H.org_kde_kwin_keystate = {
    fetchStates(object) {
      const locked = ws.kbd ? ws.kbd.locked : 0;
      send(object, "stateChanged", 0, locked & LOCK ? 2 : 0);	// capslock
      send(object, "stateChanged", 1, locked & MOD2 ? 2 : 0);	// numlock
      send(object, "stateChanged", 2, 0);	// scrolllock
    },
    destroy() {},
  };

  // xdg-foreign (v2): a handle for a window, and another program's window made a dialog of it.
  const exported = new Map();	// handle -> surface
  let handles = 0;
  H.zxdg_exporter_v2 = {
    destroy() {},
    export_toplevel(exporter, id, surfaceObject) {
      const object = ws.create(exporter.client, id, "zxdg_exported_v2", 1);
      const handle = uuid().slice(1, -1) + "-" + ++handles;
      exported.set(handle, surfaceObject.surface);
      object.onDestroy = () => exported.delete(handle);
      send(object, "handle", handle);
    },
  };
  H.zxdg_exported_v2 = {
    destroy() {},
  };
  H.zxdg_importer_v2 = {
    destroy() {},
    import_toplevel(importer, id, handle) {
      const object = ws.create(importer.client, id, "zxdg_imported_v2", 1);
      object.target = exported.get(handle) || null;
      if (!object.target) {
        send(object, "destroyed");
      }
    },
  };
  H.zxdg_imported_v2 = {
    destroy() {},
    set_parent_of(object, surfaceObject) {
      const child = surfaceObject.surface;
      const parent = object.target;
      if (child.top && parent && parent.top) {
        child.top.parent = parent;
      }
    },
  };

  // ---- the globals

  global("org_kde_plasma_shell", 8, () => {});
  global("org_kde_plasma_window_management", 18, (m) => {
    syncWindows();	// (before it is one of the managers: what is new now is announced to it below, once)
    log("plasmashell: client " + m.client.fd + " (pid " + pidOf(m.client) + ") follows the windows (version " + m.version + ")");
    managers.add(m);
    m.onDestroy = () => managers.delete(m);
    for (const rec of records.values()) {
      if (m.version >= 13) {
        send(m, "window_with_uuid", rec.id, rec.uuid);
      } else {
        send(m, "window", rec.id);
      }
    }
    sendStackingOrder(m, windows.filter(isManaged).map((w) => records.get(w).uuid));
    send(m, "show_desktop_changed", lastShowing ? 1 : 0);
  });
  global("org_kde_plasma_activation_feedback", 1, (f) => {
    feedbacks.add(f);
    f.onDestroy = () => feedbacks.delete(f);
  });
  global("org_kde_plasma_virtual_desktop_management", 2, (m) => {
    syncDesktops();
    vdManagers.add(m);
    m.onDestroy = () => vdManagers.delete(m);
    vdLast.list.forEach((d, i) => send(m, "desktop_created", d.id, i));
    send(m, "rows", vdLast.rows);
    send(m, "done");
  });
  global("kde_output_order_v1", 1, (o) => {
    send(o, "output", "PAGE-1");	// (wl_output.name: compositor.js)
    send(o, "done");
  });
  global("org_kde_kwin_blur_manager", 1, () => {});
  global("org_kde_kwin_contrast_manager", 2, () => {});
  global("org_kde_kwin_slide_manager", 1, () => {});
  global("org_kde_kwin_shadow_manager", 2, () => {});
  global("org_kde_kwin_appmenu_manager", 2, () => {});
  global("org_kde_kwin_server_decoration_palette_manager", 1, () => {});
  global("zwp_idle_inhibit_manager_v1", 1, () => {});
  global("org_kde_kwin_keystate", 5, () => {});
  global("zxdg_exporter_v2", 1, () => {});
  global("zxdg_importer_v2", 1, () => {});

  // The desktop's shell is plasmashell (WLSERVER_SHELL=plasma, `desktop wayland plasma-native`): the page is told
  // to draw no panel and no wallpaper of its own (the table's flags word), and kwin.js and notify.js leave
  // plasmashell's names on the session bus to it.
  ws.nativeShell = ws.envp.some((e) => e === "WLSERVER_SHELL=plasma");
  if (ws.nativeShell) {
    ws.table.flags(1);
    log("plasmashell: the desktop's shell is plasmashell (the page draws no panel or wallpaper of its own)");
  }
})
