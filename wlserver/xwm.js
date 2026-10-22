// X programs: Xwayland, started when the first one connects, rootless (each
// X window a window of ours), and the X window manager it needs, here.
//
// The display. At start we make X's socket, /tmp/.X11-unix/X0 (and the lock
// file /tmp/.X0-lock), ourselves and only watch it: nothing runs until an X
// program connects. Then we start Xwayland and hand it that socket
// (-listenfd), so it accepts the program that was waiting, and two socket
// pairs: one end of the first is Xwayland's Wayland connection to us
// (WAYLAND_SOCKET), whose other end we make a client here (server.js:
// addClient, marked `xwayland`, the only client told of xwayland_shell_v1);
// one end of the second is an X connection Xwayland makes a client of its own
// for the window manager (-wm), whose other end we speak X11 over (x11.js).
// Xwayland ends itself a while after its last X program has gone
// (-terminate), and the socket is watched again. (wlroots' and Sommelier's
// window managers work the same way; this one is written from the protocol
// documents: the X protocol, ICCCM, EWMH and xwayland-shell-v1.)
//
// The window manager. Rootless, Xwayland makes a wl_surface for every X
// window that is mapped on the root (the top-level ones, menus and tooltips
// too) once we have redirected the root's children (Composite, manual). It
// says which window a surface is for twice: on the Wayland side,
// xwayland_surface_v1.set_serial(serial) for the surface; on the X side, a
// WL_SURFACE_SERIAL client message with the same serial for the window (or,
// without xwayland_shell_v1, WL_SURFACE_ID with the surface's object id). The
// two come on different sockets, in either order: a window is matched with
// its surface when both are there, and from then on the surface is drawn as
// that window: a toplevel with our frame (title from _NET_WM_NAME or WM_NAME,
// app id from WM_CLASS) or, for an override-redirect window or one whose
// _NET_WM_WINDOW_TYPE is a menu, tooltip or the like, a popup at the place X
// has it, above the windows, with no frame.
//
// Coordinates: X's root is our screen, pixel for pixel (scale 1: Xwayland
// draws at scale 1, and the page doubles it on a Retina screen). An X window
// is kept where we draw it: moving a window here moves the X window
// (ConfigureWindow; xwmSync, from compositor.js's writeSlot), so that
// Xwayland's pointer coordinates and the places of menus, which X programs
// work out from their window's position, are right. Sizes go the other way
// too: a resize from our frame is a ConfigureWindow (xwmConfigure, which
// shell.js calls where it would send an xdg_toplevel.configure), within the
// window's WM_NORMAL_HINTS (minimum, maximum, resize increments: xterm
// resizes by whole characters). An X program's own ConfigureRequest is
// carried out for its size and answered (a synthetic ConfigureNotify when
// nothing changed, as ICCCM wants); its position is ours once it is mapped.
//
// Focus: activating an X window (a click, the switcher) gives it X's input
// focus (SetInputFocus, if its WM_HINTS do not say otherwise, and
// WM_TAKE_FOCUS if it asks for that), raises it among the X windows, and
// sets _NET_ACTIVE_WINDOW; activating a Wayland window takes X's focus away.
// _NET_WM_STATE is kept as the window is (maximized, fullscreen, hidden,
// focused), and a program's requests to change it (client messages) are
// done like our frame's buttons; WM_CHANGE_STATE to Iconic minimizes;
// _NET_WM_MOVERESIZE (a GTK window dragged by its own title bar) moves or
// resizes by the pointer; our close button sends WM_DELETE_WINDOW, or kills
// the client of a window that does not take it. WM_TRANSIENT_FOR makes a
// dialog open over the middle of its parent.
//
// The clipboard between X programs and Wayland ones is xselection.js's.
// Not done: drag and drop between X and Wayland programs (XDND); X windows
// of more than one screen; _NET_WM_SYNC_REQUEST; icons (_NET_WM_ICON).
(function (ws) {
  "use strict";

  const { log, sys1, sys2, sys3, sys4, SYS, H, global, screen, chrome, F } = ws;
  const X = ws.X11;
  const { EV, MASK } = X;
  const AF_UNIX = 1, SOCK_STREAM = 1, SOCK_NONBLOCK = 0o4000, SOCK_CLOEXEC = 0o2000000;
  const O_WRONLY = 1, O_CREAT = 0o100, O_TRUNC = 0o1000, O_APPEND = 0o2000, O_NONBLOCK = 0o4000, O_CLOEXEC = 0o2000000;
  const F_SETFL = 4, AT_FDCWD = -100, X_OK = 1, EEXIST = 17, EPERM = 1;
  const HN_SPAWN_DETACHED = 1;
  const EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8;
  const LOG = "/root/xwayland.log";

  const env = (name) => {
    const prefix = name + "=";
    const e = ws.envp.find((e) => e.startsWith(prefix));
    return e ? e.slice(prefix.length) : null;
  };

  ws.xdebug = env("WLSERVER_XDEBUG") === "1";
  const DISPLAY = Number(env("WLSERVER_X_DISPLAY") || 0);
  const XWAYLAND = env("WLSERVER_XWAYLAND") || "/usr/bin/Xwayland";
  const SOCKET_DIR = "/tmp/.X11-unix";
  const SOCKET = SOCKET_DIR + "/X" + DISPLAY;
  const LOCK = "/tmp/.X" + DISPLAY + "-lock";
  const TERMINATE = env("WLSERVER_X_LINGER") || "30";	// seconds Xwayland stays after its last X program

  // ---- the display's socket

  let listener = -1;
  const pid = ws.sys0(SYS.getpid);

  const makeSocket = () => {
    if (env("WLSERVER_X") === "0") {
      return false;
    }
    if (sys4(SYS.faccessat, AT_FDCWD, ws.path(XWAYLAND, ws.P0), X_OK, 0) < 0) {
      log("xwm: no " + XWAYLAND + ": no X programs");
      return false;
    }
    // Another X server on this display? (Its lock file names a process that is there.)
    const lock = ws.readText(LOCK);
    if (lock !== null) {
      const other = parseInt(lock.trim(), 10);
      if (other > 0 && other !== pid) {
        const r = sys2(SYS.kill, other, 0);
        if (r === 0 || r === -EPERM) {
          log("xwm: display :" + DISPLAY + " is another X server's (process " + other + "): no X programs here");
          return false;
        }
      }
    }
    sys3(SYS.mkdirat, AT_FDCWD, ws.path(SOCKET_DIR, ws.P0), 0o1777);
    const fd = sys4(SYS.openat, AT_FDCWD, ws.path(LOCK, ws.P0), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o444);
    if (fd >= 0) {
      ws.writeAll(fd, String(pid).padStart(10) + "\n");
      sys1(SYS.close, fd);
    }
    sys3(SYS.unlinkat, AT_FDCWD, ws.path(SOCKET, ws.P0), 0);
    const s = sys3(SYS.socket, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) {
      log("xwm: socket: error " + -s);
      return false;
    }
    const addr = ws.alloc(112);
    ws.u8().fill(0, addr, addr + 112);
    ws.dv().setUint16(addr, AF_UNIX, true);
    const raw = new TextEncoder().encode(SOCKET);
    ws.u8().set(raw, addr + 2);
    let r = sys3(SYS.bind, s, addr, 2 + raw.length + 1);
    ws.free(addr);
    if (r === 0) {
      r = sys2(SYS.listen, s, 128);
    }
    if (r < 0) {
      log("xwm: cannot listen on " + SOCKET + " (error " + -r + "): no X programs");
      sys1(SYS.close, s);
      return false;
    }
    listener = s;
    return true;
  };

  // ---- Xwayland

  let xpid = 0;			// Xwayland's process, while it runs
  let xclient = null;		// its Wayland connection (server.js's client)
  let conn = null;		// our X connection (x11.js)
  let starts = [];		// when it was started lately (a program that makes it fail at once is not retried forever)

  const socketPair = () => {
    const pair = ws.alloc(8);
    const r = sys4(SYS.socketpair, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair);
    const v = ws.dv();
    const fds = r < 0 ? null : [v.getInt32(pair, true), v.getInt32(pair + 4, true)];
    ws.free(pair);
    return fds;
  };

  // The desktop shell is not an X program. With DISPLAY set (the programs it starts, the panel's launchers, inherit
  // it, so it is) plasmashell opens one X connection of its own at start: its KScreen XRandR backend asks for the
  // RANDR extension (the first request, seen with a stand-in X server) and gives up at once without it. That one
  // connection would start Xwayland for the whole session, and with the shell's connection open it would never end.
  // Until the shell has made it, a pending connection is therefore taken here and its process looked at (SO_PEERCRED):
  // the shell's is closed, the display stays unserved; anyone else's starts Xwayland as before and is carried to it
  // (bridge: the first X program in the first minute of a session, before the shell has made its connection, a
  // connection that cannot pass descriptors). The window ends with the shell's connection, or after WINDOW seconds.
  const WINDOW = 60;
  let shellProbed = env("WLSERVER_SHELL") !== "plasma" || env("WLSERVER_X_PEEK") === "0";
  const t0 = performance.now();
  let carried = -1;		// an X program's connection taken before Xwayland ran, to be carried to it
  const peerCommand = (fd) => {
    const b = ws.alloc(16), l = ws.alloc(4);
    ws.dv().setInt32(l, 12, true);
    const r = ws.sys5(SYS.getsockopt, fd, 1 /* SOL_SOCKET */, 17 /* SO_PEERCRED */, b, l);
    const peer = r === 0 ? ws.dv().getInt32(b, true) : 0;
    ws.free(b);
    ws.free(l);
    return { pid: peer, cmd: peer > 0 ? (ws.readText("/proc/" + peer + "/cmdline") || "").replace(/\0/g, " ").trim() : "" };
  };

  // Carries an accepted X connection (a) to the display's socket once Xwayland listens: bytes both ways until one ends.
  const bridge = (a) => {
    const c = sys3(SYS.socket, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    const addr = ws.alloc(112);
    ws.u8().fill(0, addr, addr + 112);
    ws.dv().setUint16(addr, AF_UNIX, true);
    const raw = new TextEncoder().encode(SOCKET);
    ws.u8().set(raw, addr + 2);
    const r = c < 0 ? c : sys3(SYS.connect, c, addr, 2 + raw.length + 1);
    ws.free(addr);
    if (r < 0) {
      log("xwm: cannot reach the display's socket for the first X program (error " + -r + ")");
      sys1(SYS.close, a);
      if (c >= 0) {
        sys1(SYS.close, c);
      }
      return;
    }
    const buf = ws.alloc(16384);
    let open = true;
    const end = () => {
      if (!open) {
        return;
      }
      open = false;
      ws.unwatchFd(a);
      ws.unwatchFd(c);
      sys1(SYS.close, a);
      sys1(SYS.close, c);
      ws.free(buf);
    };
    const move = (from, to) => () => {
      while (open) {
        const n = sys3(SYS.read, from, buf, 16384);
        if (n === -4) {
          continue;
        }
        if (n === -11) {
          return;
        }
        if (n <= 0 || ws.writeAll(to, ws.u8().slice(buf, buf + n)) < 0) {
          end();
          return;
        }
      }
    };
    ws.watchFd(a, move(a, c));
    ws.watchFd(c, move(c, a));
  };

  const onPending = () => {
    if (!shellProbed && performance.now() - t0 > WINDOW * 1000) {
      shellProbed = true;
    }
    if (!shellProbed) {
      const fd = sys4(SYS.accept4, listener, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) {
        return;	// (nothing there after all: the level-triggered watch fires again when something is)
      }
      const peer = peerCommand(fd);
      if (/(^|\/)plasmashell$/.test(peer.cmd.split(" ")[0])) {
        shellProbed = true;
        sys1(SYS.close, fd);
        log("xwm: the desktop shell (process " + peer.pid + ") looked at the X display and was turned away: Xwayland stays off until an X program asks");
        return;
      }
      log("xwm: an X program (process " + peer.pid + ") connected before the shell did: carried to Xwayland");
      shellProbed = true;
      carried = fd;
    }
    ws.unwatchFd(listener);	// (Xwayland accepts on it from now on)
    const t = performance.now();
    starts = starts.filter((s) => t - s < 30000);
    if (starts.length >= 3) {
      log("xwm: Xwayland was started three times in 30 seconds: not again (see " + LOG + ")");
      if (carried >= 0) {
        sys1(SYS.close, carried);
        carried = -1;
      }
      return;
    }
    starts.push(t);
    start();
    if (carried >= 0) {
      const a = carried;
      carried = -1;
      if (xpid) {
        bridge(a);
      } else {
        sys1(SYS.close, a);
      }
    }
  };

  const start = () => {
    const wl = socketPair(), wm = socketPair();
    if (!wl || !wm) {
      log("xwm: socketpair failed: Xwayland not started");
      return;
    }
    const logFd = sys4(SYS.openat, AT_FDCWD, ws.path(LOG, ws.P0), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0o644);
    const environment = ws.envp.filter((e) => !/^(HOSTNODE_HOME|WLSERVER_[A-Z_]*|WAYLAND_DISPLAY|WAYLAND_SOCKET|DISPLAY)=/.test(e));
    environment.push("WAYLAND_SOCKET=3");
    // Child descriptors: 0 nothing, 1 and 2 the log, 3 its Wayland connection, 4 the window manager's X
    // connection, 5 the display's socket.
    const argv = ["Xwayland", ":" + DISPLAY, "-rootless", "-shm", "-wm", "4", "-listenfd", "5", "-nolisten", "tcp",
                  "-terminate", TERMINATE];
    const child = ws.spawnWith(argv, environment, [-1, logFd, logFd, wl[1], wm[1], listener], HN_SPAWN_DETACHED);
    sys1(SYS.close, wl[1]);
    sys1(SYS.close, wm[1]);
    if (logFd >= 0) {
      sys1(SYS.close, logFd);
    }
    if (child < 0) {
      log("xwm: cannot start " + XWAYLAND + " (error " + -child + ")");
      sys1(SYS.close, wl[0]);
      sys1(SYS.close, wm[0]);
      ws.watchFd(listener, onPending);
      return;
    }
    xpid = child;
    log("xwm: an X program connected: Xwayland started (process " + child + ")");
    sys3(SYS.fcntl, wl[0], F_SETFL, O_NONBLOCK);
    sys3(SYS.fcntl, wm[0], F_SETFL, O_NONBLOCK);
    xclient = ws.addClient(wl[0]);
    xclient.xwayland = true;
    const c = new X.Connection(wm[0], (m) => event(c, m), () => closed(c));
    conn = c;
    ws.watchFd(wm[0], () => c.readable());
    c.start(() => ready(c));
  };

  // Our X connection is gone (Xwayland ended, or broke it).
  const closed = (c) => {
    ws.unwatchFd(c.fd);
    sys1(SYS.close, c.fd);
    if (conn === c) {
      conn = null;
    }
    for (const w of [...wins.values()]) {
      if (w.surface) {
        unpair(w);
      }
    }
    wins.clear();
    focusWin = 0;
    if (ws.xselectionGone) {
      ws.xselectionGone();
    }
  };

  // From apps.js (every second): a child that ended.
  ws.childExited = (child, status) => {
    if (child !== xpid) {
      return;
    }
    log("xwm: Xwayland ended (status " + (status >> 8 & 0xff) + (status & 0x7f ? ", signal " + (status & 0x7f) : "")
        + "); it starts again when an X program connects");
    xpid = 0;
    if (conn) {
      conn.close();
    }
    xclient = null;
    if (listener >= 0) {
      ws.watchFd(listener, onPending);
    }
  };

  // ---- the window manager's start

  const ATOMS = [
    "WL_SURFACE_ID", "WL_SURFACE_SERIAL", "WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_TAKE_FOCUS", "WM_STATE",
    "WM_CHANGE_STATE", "WM_NAME", "WM_CLASS", "WM_NORMAL_HINTS", "WM_HINTS", "WM_TRANSIENT_FOR", "UTF8_STRING",
    "STRING", "ATOM", "WINDOW", "CARDINAL", "_NET_WM_NAME", "_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK",
    "_NET_ACTIVE_WINDOW", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_FOCUSED", "_NET_WM_STATE_MODAL",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
    "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_TOOLTIP",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_COMBO", "_NET_WM_WINDOW_TYPE_DND",
    "_NET_WM_MOVERESIZE", "_NET_CLOSE_WINDOW", "_NET_FRAME_EXTENTS", "_MOTIF_WM_HINTS", "_GTK_FRAME_EXTENTS", "WM_S0",
    // virtual desktops and KWin's window states (desktops.js, shell.js)
    "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP", "_NET_DESKTOP_NAMES", "_NET_DESKTOP_VIEWPORT", "_NET_DESKTOP_GEOMETRY",
    "_NET_DESKTOP_LAYOUT", "_NET_WORKAREA", "_NET_SHOWING_DESKTOP", "_NET_WM_DESKTOP", "_NET_CLIENT_LIST",
    "_NET_CLIENT_LIST_STACKING", "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_BELOW", "_NET_WM_STATE_SHADED",
    "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER", "_NET_WM_STATE_DEMANDS_ATTENTION",
    "WM_WINDOW_ROLE",
    // the clipboard's (xselection.js)
    "CLIPBOARD", "PRIMARY", "TARGETS", "TIMESTAMP", "INCR", "TEXT", "MULTIPLE", "SAVE_TARGETS", "DELETE",
    "text/plain;charset=utf-8", "text/plain", "_WLSERVER_SELECTION",
  ];
  let A = {};			// atoms by name
  let root = 0;
  let check = 0;		// our window for _NET_SUPPORTING_WM_CHECK

  const ready = (c) => {
    root = c.setup.screen.root;
    let left = 3;
    const next = () => {
      if (--left === 0) {
        manage(c);
      }
    };
    c.internAtoms(ATOMS, (atoms) => {
      A = atoms;
      ws.xatoms = atoms;
      next();
    });
    c.queryExtension("XFIXES", (e) => {
      if (e) {
        c.ext("XFIXES", 0, (w) => w.u32(5).u32(0), () => next());	// XFixesQueryVersion 5.0 (before any other)
      } else {
        log("xwm: no XFIXES: the clipboard of X programs is not seen");
        next();
      }
    });
    c.queryExtension("Composite", (e) => {
      if (!e) {
        log("xwm: no Composite extension: Xwayland cannot show X windows rootless");
        next();
        return;
      }
      c.ext("Composite", 0, (w) => w.u32(0).u32(4), () => next());	// CompositeQueryVersion 0.4
    });
  };

  const manage = (c) => {
    // The root's children are ours to place, and Xwayland makes surfaces of them once they are redirected.
    c.selectInput(root, MASK.SubstructureRedirect | MASK.SubstructureNotify | MASK.PropertyChange);
    c.ext("Composite", 2, (w) => w.u32(root).u8(1).u8(0).u16(0));	// RedirectSubwindows(root, Manual)
    check = c.createInputOnly(root, -100, -100, 1, 1, 0);
    c.changeProperty(check, A._NET_SUPPORTING_WM_CHECK, A.WINDOW, 32, [check]);
    c.changeProperty(check, A._NET_WM_NAME, A.UTF8_STRING, 8, "wlserver");
    c.changeProperty(root, A._NET_SUPPORTING_WM_CHECK, A.WINDOW, 32, [check]);
    c.changeProperty(root, A._NET_SUPPORTED, A.ATOM, 32, [
      A._NET_WM_NAME, A._NET_SUPPORTING_WM_CHECK, A._NET_ACTIVE_WINDOW, A._NET_WM_STATE,
      A._NET_WM_STATE_MAXIMIZED_VERT, A._NET_WM_STATE_MAXIMIZED_HORZ, A._NET_WM_STATE_FULLSCREEN,
      A._NET_WM_STATE_HIDDEN, A._NET_WM_STATE_FOCUSED, A._NET_WM_WINDOW_TYPE, A._NET_WM_MOVERESIZE,
      A._NET_CLOSE_WINDOW, A._NET_FRAME_EXTENTS,
      A._NET_NUMBER_OF_DESKTOPS, A._NET_CURRENT_DESKTOP, A._NET_DESKTOP_NAMES, A._NET_DESKTOP_VIEWPORT,
      A._NET_DESKTOP_GEOMETRY, A._NET_DESKTOP_LAYOUT, A._NET_WORKAREA, A._NET_SHOWING_DESKTOP, A._NET_WM_DESKTOP,
      A._NET_CLIENT_LIST, A._NET_CLIENT_LIST_STACKING, A._NET_WM_STATE_ABOVE, A._NET_WM_STATE_BELOW,
      A._NET_WM_STATE_SHADED, A._NET_WM_STATE_STICKY, A._NET_WM_STATE_SKIP_TASKBAR, A._NET_WM_STATE_SKIP_PAGER,
      A._NET_WM_STATE_DEMANDS_ATTENTION,
    ]);
    c.changeProperty(root, A._NET_ACTIVE_WINDOW, A.WINDOW, 32, [0]);
    rootDesktops = "";
    ws.xwmDesktops();
    // The window manager's selection (ICCCM 2.8): Xwayland given -wm waits for someone to take it before it
    // accepts X programs on the sockets it was given (-listenfd).
    c.setSelectionOwner(check, A.WM_S0, 0);
    if (ws.xselectionStart) {
      ws.xselectionStart(c, root, A);
    }
    // Windows that are there already (a program faster than us): taken as if they had just come.
    c.request(X.OP.QueryTree, 0, (w) => w.u32(root), (m) => {
      if (!m) {
        return;
      }
      const n = m.u16(16);
      for (let i = 0; i < n; i++) {
        adopt(c, m.u32(32 + 4 * i));
      }
    });
    log("xwm: managing the X display :" + DISPLAY);
  };

  const adopt = (c, id) => {
    if (id === check || wins.has(id) || (ws.xselectionWindow && id === ws.xselectionWindow())) {
      return;
    }
    c.request(X.OP.GetWindowAttributes, 0, (w) => w.u32(id), (attrs) => {
      if (!attrs) {
        return;
      }
      c.request(X.OP.GetGeometry, 0, (w) => w.u32(id), (g) => {
        if (!g || wins.has(id) || c !== conn) {
          return;
        }
        const w = newWin(id, g.i16(12), g.i16(14), g.u16(16), g.u16(18), !!attrs.u8(27));
        if (attrs.u8(26) === 2) {	// viewable: mapped before we were there
          if (w.or) {
            w.mapped = true;
            tryPair(w);
          } else {
            readProps(w, PROPS, () => {
              w.managed = true;
              w.mapped = true;
              c.changeProperty(w.id, A.WM_STATE, A.WM_STATE, 32, [1, 0]);
              tryPair(w);
            });
          }
        }
      });
    });
  };

  // ---- windows

  const wins = new Map();	// X window id -> what we know of it
  let focusWin = 0;		// the X window with X's focus (as we gave it)

  const newWin = (id, x, y, w, h, or) => {
    const win = {
      id, x, y, w, h, or, mapped: false, managed: false, surface: null, serial: null, wlId: 0,
      wmName: "", netName: null, inst: "", cls: "", hints: {}, input: true, protocols: new Set(), transientFor: 0,
      types: [], motifDeco: true, netStateWanted: new Set(), gtkExtents: null, sent: null, lastPos: null,
      netState: "", iconic: false, role: "", urgent: false, desktopWanted: null, netDesktop: null,
    };
    wins.set(id, win);
    conn.selectInput(id, MASK.PropertyChange | MASK.FocusChange);
    return win;
  };

  // A window that is a menu, a tooltip and the like: drawn where it is, above everything, with no frame.
  const isPopup = (w) => w.or || w.types.some((t) => t === A._NET_WM_WINDOW_TYPE_MENU || t === A._NET_WM_WINDOW_TYPE_DROPDOWN_MENU
    || t === A._NET_WM_WINDOW_TYPE_POPUP_MENU || t === A._NET_WM_WINDOW_TYPE_TOOLTIP
    || t === A._NET_WM_WINDOW_TYPE_NOTIFICATION || t === A._NET_WM_WINDOW_TYPE_COMBO || t === A._NET_WM_WINDOW_TYPE_DND);

  const title = (w) => w.netName !== null && w.netName !== "" ? w.netName : w.wmName;
  const extents = (w) => w.gtkExtents || [0, 0, 0, 0];	// left, right, top, bottom

  // ---- properties

  const PROPS = ["WM_NAME", "_NET_WM_NAME", "WM_CLASS", "WM_NORMAL_HINTS", "WM_HINTS", "WM_PROTOCOLS", "WM_TRANSIENT_FOR",
                 "_NET_WM_WINDOW_TYPE", "_NET_WM_STATE", "_MOTIF_WM_HINTS", "_GTK_FRAME_EXTENTS", "WM_WINDOW_ROLE",
                 "_NET_WM_DESKTOP"];

  // Read the properties named (atom names) of a window into what we know of it; done() when all are in.
  const readProps = (w, names, done) => {
    let left = names.length;
    for (const name of names) {
      const atom = A[name];
      conn.getProperty(w.id, atom, (p) => {
        parseProp(w, name, p);
        if (--left === 0 && done) {
          done();
        }
      }, { max: 65536 });
    }
  };

  const parseProp = (w, name, p) => {
    const words = X.propertyWords(p);
    switch (name) {
    case "WM_NAME":
      w.wmName = X.propertyText(p, A.UTF8_STRING) || "";
      break;
    case "_NET_WM_NAME":
      w.netName = p ? X.propertyText(p, A.UTF8_STRING) : null;
      break;
    case "WM_CLASS": {
      const parts = p && p.format === 8 ? new TextDecoder("latin1").decode(p.data).split("\0") : [];
      w.inst = parts[0] || "";
      w.cls = parts[1] || "";
      break;
    }
    case "WM_NORMAL_HINTS": {
      // flags, x, y, width, height (obsolete), min w h, max w h, inc w h, min aspect, max aspect, base w h, gravity
      const h = {};
      if (words.length >= 15) {
        const f = words[0];
        if (f & 16) {
          h.min = [words[5] | 0, words[6] | 0];
        }
        if (f & 32) {
          h.max = [words[7] | 0, words[8] | 0];
        }
        if (f & 64) {
          h.inc = [words[9] | 0, words[10] | 0];
        }
        if ((f & 256) && words.length >= 17) {
          h.base = [words[15] | 0, words[16] | 0];
        }
        if (f & 5) {
          h.position = true;	// (USPosition or PPosition)
        }
      }
      w.hints = h;
      break;
    }
    case "WM_HINTS":
      // flags, input, initial state, ...: InputHint (flag 1) false means the program takes no focus from us.
      w.input = words.length >= 2 && (words[0] & 1) ? words[1] !== 0 : true;
      w.urgent = words.length >= 1 && (words[0] & 256) !== 0;	// (UrgencyHint: the task button flashes)
      break;
    case "WM_WINDOW_ROLE":
      w.role = p ? X.propertyText(p, A.STRING) || "" : "";
      break;
    case "_NET_WM_DESKTOP":
      w.desktopWanted = words.length ? words[0] >>> 0 : null;	// (asked for before the window maps; ours after)
      break;
    case "WM_PROTOCOLS":
      w.protocols = new Set(words);
      break;
    case "WM_TRANSIENT_FOR":
      w.transientFor = words[0] || 0;
      break;
    case "_NET_WM_WINDOW_TYPE":
      w.types = words;
      break;
    case "_NET_WM_STATE":
      w.netStateWanted = new Set(words);
      break;
    case "_MOTIF_WM_HINTS":
      // flags, functions, decorations, input mode, status: decorations (flag 2) 0 is "none".
      w.motifDeco = !(words.length >= 3 && (words[0] & 2) && words[2] === 0);
      break;
    case "_GTK_FRAME_EXTENTS":
      w.gtkExtents = words.length >= 4 ? words.slice(0, 4).map((x) => x | 0) : null;
      break;
    }
  };

  // What we know of a window changed: its surface is told.
  const applyProps = (w) => {
    const s = w.surface;
    if (!s) {
      return;
    }
    s.title = title(w);
    s.appId = w.cls || w.inst;
    if (s.top && !!s.top.attention !== w.urgent) {
      s.top.attention = w.urgent;
    }
    if (s.top) {
      s.top.min = w.hints.min ? w.hints.min.slice() : [0, 0];
      s.top.max = w.hints.max ? w.hints.max.slice() : [0, 0];
      const parent = w.transientFor && wins.get(w.transientFor);
      s.top.parent = parent && parent.surface ? parent.surface : null;
    }
    s.dirty = true;
  };

  // ---- a window and its surface

  const serials = new Map();	// serial (xwayland_surface_v1.set_serial) -> surface

  const tryPair = (w) => {
    if (w.surface || !w.mapped) {
      return;
    }
    let s = null;
    if (w.serial !== null) {
      s = serials.get(w.serial) || null;
    } else if (w.wlId && xclient) {
      const o = xclient.objects.get(w.wlId);
      s = o && o.interface === "wl_surface" ? o.surface : null;
    }
    if (s && !s.destroyed && !s.xwin) {
      pair(w, s);
    }
  };

  const pair = (w, s) => {
    w.surface = s;
    s.xwin = w;
    s.role = "xwayland";
    if (!isPopup(w)) {
      s.top = {
        wx: 0, wy: 0, size: [0, 0], min: [0, 0], max: [0, 0], resize: null, anchor: null, anchorDone: false,
        anchorEnd: false, deco: null, parent: null, saved: null, savedFull: null, placed: false, sentCapabilities: true,
      };
      const e = extents(w);
      s.top.size = [w.w - e[0] - e[1], w.h - e[2] - e[3]];
      if (w.lastPos) {
        [s.top.wx, s.top.wy] = w.lastPos;
        s.top.placed = true;
      } else if (w.hints.position && w.transientFor === 0) {
        // (a program that asks for its place: within the screen, as far as it goes)
        s.top.wx = w.x + e[0];
        s.top.wy = Math.max(ws.decorated(s) ? chrome.title : 0, w.y + e[2]);
        s.top.placed = true;
      }
    }
    applyProps(w);
    {
      log("xwm: window " + w.id + " is surface " + s.id + (s.top ? " (a window)" : " (a popup)") + " \"" + s.title + "\"");
    }
    if (s.top) {
      // States and the desktop the program asked for before mapping (EWMH: _NET_WM_STATE, _NET_WM_DESKTOP).
      const t = s.top, want = w.netStateWanted;
      t.above = want.has(A._NET_WM_STATE_ABOVE);
      t.below = !t.above && want.has(A._NET_WM_STATE_BELOW);
      t.skipTaskbar = want.has(A._NET_WM_STATE_SKIP_TASKBAR);
      t.skipPager = want.has(A._NET_WM_STATE_SKIP_PAGER);
      t.shaded = want.has(A._NET_WM_STATE_SHADED) && ws.xwmDecorated(s);
      t.attention = w.urgent || want.has(A._NET_WM_STATE_DEMANDS_ATTENTION);
      if (want.has(A._NET_WM_STATE_STICKY) || w.desktopWanted === 0xffffffff) {
        t.vd = null;
      } else if (w.desktopWanted !== null && ws.desktops && ws.desktops.list[w.desktopWanted]) {
        t.vd = ws.desktops.list[w.desktopWanted].id;
      }
    }
    ws.showCurrent(s);
    clientList();
    if (s.top) {
      if (w.netStateWanted.has(A._NET_WM_STATE_FULLSCREEN)) {
        ws.setFullscreen(s, true);
      } else if (w.netStateWanted.has(A._NET_WM_STATE_MAXIMIZED_VERT) && w.netStateWanted.has(A._NET_WM_STATE_MAXIMIZED_HORZ)) {
        ws.setMaximized(s, true);
      }
    }
  };

  const unpair = (w) => {
    const s = w.surface;
    if (!s) {
      return;
    }
    if (s.top) {
      w.lastPos = [s.top.wx, s.top.wy];
    }
    ws.unmapSurface(s);		// (before the window is forgotten: the next one is activated)
    s.xwin = null;
    s.top = null;
    s.flags &= ~(F.ACTIVATED | F.MAXIMIZED | F.FULLSCREEN | F.MINIMIZED);
    w.surface = null;
    w.netDesktop = null;
    clientList();
  };

  // From compositor.js: a surface of Xwayland's is going.
  ws.xwmSurfaceGone = (s) => {
    if (s.xserial !== undefined && serials.get(s.xserial) === s) {
      serials.delete(s.xserial);
    }
    const w = s.xwin;
    if (w) {
      if (s.top) {
        w.lastPos = [s.top.wx, s.top.wy];
      }
      w.surface = null;
      s.xwin = null;
      s.top = null;
    }
  };

  // From compositor.js: a new surface of Xwayland's (WL_SURFACE_ID may have named it already).
  ws.xwmNewSurface = (s) => {
    for (const w of wins.values()) {
      if (!w.surface && w.wlId === s.object.id && w.serial === null) {
        tryPair(w);
      }
    }
  };

  // ---- what compositor.js and shell.js ask

  ws.xwmPlaceable = (s) => {
    const w = s.xwin;
    return !!(w && w.mapped && wins.get(w.id) === w);
  };

  ws.xwmPos = (s) => {
    const w = s.xwin;
    if (s.top) {
      const g = ws.geometry(s);
      return [s.top.wx - g[0], s.top.wy - g[1]];
    }
    return [w.x, w.y];
  };

  ws.xwmGeometry = (s) => {
    const w = s.xwin;
    if (!w.gtkExtents) {
      return null;
    }
    const [sw, sh] = ws.size(s);
    const e = w.gtkExtents;
    return [e[0], e[2], Math.max(1, sw - e[0] - e[1]), Math.max(1, sh - e[2] - e[3])];
  };

  ws.xwmDecorated = (s) => {
    const w = s.xwin;
    return w.motifDeco && !w.gtkExtents && !w.types.includes(A._NET_WM_WINDOW_TYPE_SPLASH);
  };

  // A popup's parent (for the page: it is hidden with its window): the window it is for, or the active X one.
  ws.xwmParent = (s) => {
    const w = s.xwin;
    const p = w.transientFor && wins.get(w.transientFor);
    if (p && p.surface) {
      return p.surface.id;
    }
    const f = focusWin && wins.get(focusWin);
    return f && f.surface ? f.surface.id : 0;
  };

  // The X window to where it is drawn (from writeSlot).
  ws.xwmSync = (s) => {
    const w = s.xwin;
    if (!s.top || !conn) {
      return;
    }
    const [x, y] = ws.xwmPos(s);
    if (!w.sent || w.sent.x !== x || w.sent.y !== y) {
      conn.configureWindow(w.id, { x, y });
      w.sent = Object.assign({}, w.sent || { w: w.w, h: w.h }, { x, y });
    }
  };

  // A size within the window's WM_NORMAL_HINTS.
  const hinted = (w, width, height) => {
    const h = w.hints;
    if (h.min) {
      width = Math.max(width, h.min[0]);
      height = Math.max(height, h.min[1]);
    }
    if (h.max && h.max[0] > 0) {
      width = Math.min(width, h.max[0]);
    }
    if (h.max && h.max[1] > 0) {
      height = Math.min(height, h.max[1]);
    }
    if (h.inc && (h.inc[0] > 1 || h.inc[1] > 1)) {
      const base = h.base || h.min || [0, 0];
      if (h.inc[0] > 1 && width > base[0]) {
        width = base[0] + Math.floor((width - base[0]) / h.inc[0]) * h.inc[0];
      }
      if (h.inc[1] > 1 && height > base[1]) {
        height = base[1] + Math.floor((height - base[1]) / h.inc[1]) * h.inc[1];
      }
    }
    return [Math.max(1, width), Math.max(1, height)];
  };

  // shell.js's configure for an X window: its place and size as the shell has them, by ConfigureWindow.
  // Returns true if something was sent.
  const configure = (s) => {
    const t = s.top, w = s.xwin;
    if (!t || !conn) {
      return false;
    }
    const e = extents(w);
    let [gw, gh] = t.size;
    if (!(gw > 0 && gh > 0)) {
      gw = w.w - e[0] - e[1];
      gh = w.h - e[2] - e[3];
    }
    if (s.flags & F.FULLSCREEN) {
      gw = screen.width;
      gh = screen.height;
    } else if (s.flags & F.MAXIMIZED) {
      gw = ws.workArea()[2];
      gh = ws.workArea()[3] - (ws.decorated(s) ? chrome.title : 0);
    } else {
      [gw, gh] = hinted(w, gw, gh);
    }
    // A resize from the left or top keeps the right or bottom edge where it was.
    if (t.anchor) {
      if (t.anchor.edges & EDGE_LEFT) {
        t.wx = t.anchor.right - gw;
      }
      if (t.anchor.edges & EDGE_TOP) {
        t.wy = t.anchor.bottom - gh;
      }
      if (!t.resize) {
        t.anchor = null;
      }
    }
    const x = t.wx - e[0], y = t.wy - e[2], width = gw + e[0] + e[1], height = gh + e[2] + e[3];
    netState(w, s);
    if (w.sent && w.sent.x === x && w.sent.y === y && w.sent.w === width && w.sent.h === height) {
      return false;
    }
    conn.configureWindow(w.id, { x, y, width, height, border: 0 });
    w.sent = { x, y, w: width, h: height };
    ws.dirtyTree(s);
    return true;
  };
  ws.xwmConfigure = configure;

  // _NET_WM_STATE (and WM_STATE's iconic or normal) as the window is now.
  const netState = (w, s) => {
    const list = [];
    if (s.flags & F.MAXIMIZED) {
      list.push(A._NET_WM_STATE_MAXIMIZED_VERT, A._NET_WM_STATE_MAXIMIZED_HORZ);
    }
    if (s.flags & F.FULLSCREEN) {
      list.push(A._NET_WM_STATE_FULLSCREEN);
    }
    if (s.flags & F.MINIMIZED) {
      list.push(A._NET_WM_STATE_HIDDEN);
    }
    if (s.flags & F.ACTIVATED) {
      list.push(A._NET_WM_STATE_FOCUSED);
    }
    const t = s.top;
    if (t) {
      for (const [on, atom] of [[t.above, A._NET_WM_STATE_ABOVE], [t.below, A._NET_WM_STATE_BELOW],
                                [t.shaded, A._NET_WM_STATE_SHADED], [t.vd === null, A._NET_WM_STATE_STICKY],
                                [t.skipTaskbar, A._NET_WM_STATE_SKIP_TASKBAR], [t.skipPager, A._NET_WM_STATE_SKIP_PAGER],
                                [t.attention, A._NET_WM_STATE_DEMANDS_ATTENTION]]) {
        if (on) {
          list.push(atom);
        }
      }
      const d = ws.windowDesktop ? ws.windowDesktop(s) : 0;
      const nd = d < 0 ? 0xffffffff : d;
      if (nd !== w.netDesktop) {
        w.netDesktop = nd;
        conn.changeProperty(w.id, A._NET_WM_DESKTOP, A.CARDINAL, 32, [nd]);
      }
    }
    const key = list.join(",");
    if (key !== w.netState) {
      w.netState = key;
      conn.changeProperty(w.id, A._NET_WM_STATE, A.ATOM, 32, list);
    }
    const iconic = !!(s.flags & F.MINIMIZED);
    if (iconic !== w.iconic) {
      w.iconic = iconic;
      conn.changeProperty(w.id, A.WM_STATE, A.WM_STATE, 32, [iconic ? 3 : 1, 0]);
    }
  };

  // The root's desktop properties (EWMH: the number of desktops, the current one, their names, the grid, the
  // usable area of each, whether the desktop is shown), when they changed.
  let rootDesktops = "";
  ws.xwmDesktops = () => {
    if (!conn || !root || !ws.desktops || !A._NET_NUMBER_OF_DESKTOPS) {
      return;
    }
    const d = ws.desktops, n = d.list.length;
    const a = ws.workArea();
    const key = JSON.stringify([d.list, d.current, d.rows, d.showing, a, screen.width, screen.height]);
    if (key === rootDesktops) {
      return;
    }
    rootDesktops = key;
    conn.changeProperty(root, A._NET_NUMBER_OF_DESKTOPS, A.CARDINAL, 32, [n]);
    conn.changeProperty(root, A._NET_CURRENT_DESKTOP, A.CARDINAL, 32, [d.current]);
    const names = new TextEncoder().encode(d.list.map((x) => x.name + "\0").join(""));
    conn.changeProperty(root, A._NET_DESKTOP_NAMES, A.UTF8_STRING, 8, names);
    conn.changeProperty(root, A._NET_DESKTOP_GEOMETRY, A.CARDINAL, 32, [screen.width, screen.height]);
    conn.changeProperty(root, A._NET_DESKTOP_VIEWPORT, A.CARDINAL, 32, new Array(2 * n).fill(0));
    const work = [];
    for (let i = 0; i < n; i++) {
      work.push(a[0], a[1], a[2], a[3]);
    }
    conn.changeProperty(root, A._NET_WORKAREA, A.CARDINAL, 32, work);
    // (orientation horizontal, columns, rows, starting at the top left: KWin's VirtualDesktopManager::updateRootInfo)
    conn.changeProperty(root, A._NET_DESKTOP_LAYOUT, A.CARDINAL, 32, [0, Math.ceil(n / d.rows), d.rows, 0]);
    conn.changeProperty(root, A._NET_SHOWING_DESKTOP, A.CARDINAL, 32, [d.showing ? 1 : 0]);
    // Every X window's _NET_WM_DESKTOP (an index may have moved with a desktop added or taken away).
    for (const w of wins.values()) {
      if (w.surface && w.surface.top) {
        netState(w, w.surface);
      }
    }
  };
  ws.workAreaChanged = () => {
    if (ws.xwmDesktops) {
      ws.xwmDesktops();
    }
    if (ws.table && ws.table.workArea) {
      ws.table.workArea(ws.workArea());
    }
  };
  // shell.js: a window's states changed.
  ws.xwmStates = (s) => {
    if (conn && s.xwin && s.top) {
      netState(s.xwin, s);
    }
  };
  // The X windows we manage, in the order they came and from the bottom up (EWMH's client lists).
  let lastClients = "";
  const clientList = () => {
    if (!conn || !root || !A._NET_CLIENT_LIST) {
      return;
    }
    const managed = [...wins.values()].filter((w) => w.surface && w.surface.top);
    const stacking = ws.windows.filter((s) => s.xwin && s.top).map((s) => s.xwin.id);
    const key = managed.map((w) => w.id).join(",") + ";" + stacking.join(",");
    if (key === lastClients) {
      return;
    }
    lastClients = key;
    conn.changeProperty(root, A._NET_CLIENT_LIST, A.WINDOW, 32, managed.map((w) => w.id));
    conn.changeProperty(root, A._NET_CLIENT_LIST_STACKING, A.WINDOW, 32, stacking);
  };
  // What a window rule matches an X window by (rules.js): KWin's resource class and name are WM_CLASS's, in lower case.
  ws.xwmDescribe = (s) => {
    const w = s.xwin;
    const type = w.types.includes(A._NET_WM_WINDOW_TYPE_DIALOG) || (w.transientFor && !w.types.length) ? "dialog"
      : w.types.includes(A._NET_WM_WINDOW_TYPE_UTILITY) ? "utility" : w.types.includes(A._NET_WM_WINDOW_TYPE_TOOLBAR) ? "toolbar"
      : w.types.includes(A._NET_WM_WINDOW_TYPE_SPLASH) ? "splash" : "normal";
    return { cls: (w.cls || "").toLowerCase(), name: (w.inst || "").toLowerCase(), title: title(w), role: w.role || "", type };
  };

  // X's input focus to the window that was made active (or away from X, for a Wayland window or none).
  ws.xwmFocus = (s) => {
    if (!conn) {
      return;
    }
    const w = s && s.xwin;
    if (w && s.top) {
      if (w.input) {
        conn.setInputFocus(w.id, 1, 0);	// (revert to PointerRoot; CurrentTime)
      }
      if (w.protocols.has(A.WM_TAKE_FOCUS)) {
        conn.clientMessage(w.id, A.WM_PROTOCOLS, [A.WM_TAKE_FOCUS, 0]);
      }
      conn.configureWindow(w.id, { stack: 0 });	// above the other X windows
      clientList();
      if (focusWin !== w.id) {
        focusWin = w.id;
        conn.changeProperty(root, A._NET_ACTIVE_WINDOW, A.WINDOW, 32, [w.id]);
      }
    } else if (focusWin) {
      focusWin = 0;
      conn.setInputFocus(0, 1, 0);
      conn.changeProperty(root, A._NET_ACTIVE_WINDOW, A.WINDOW, 32, [0]);
    }
  };

  ws.xwmClose = (s) => {
    const w = s.xwin;
    if (!conn || !w) {
      return;
    }
    if (w.protocols.has(A.WM_DELETE_WINDOW)) {
      conn.clientMessage(w.id, A.WM_PROTOCOLS, [A.WM_DELETE_WINDOW, 0]);
    } else {
      conn.killClient(w.id);
    }
  };

  // A ConfigureNotify of our own making: what the window's geometry is (ICCCM 4.1.5, for a request that changed
  // nothing).
  const syntheticConfigure = (w) => {
    const g = w.sent || { x: w.x, y: w.y, w: w.w, h: w.h };
    conn.sendEvent(w.id, false, MASK.StructureNotify, (b) => {
      b.u8(EV.ConfigureNotify).u8(0).u16(0).u32(w.id).u32(w.id).u32(0);
      b.i16(g.x).i16(g.y).u16(g.w).u16(g.h).u16(0).u8(0).u8(0);
    });
  };

  // ---- events

  const EVENT_NAMES = {};
  for (const name of Object.keys(EV)) {
    EVENT_NAMES[EV[name]] = name;
  }

  const event = (c, m) => {
    if (c !== conn) {
      return;
    }
    const code = m.u8(0) & 0x7f;
    if (ws.xdebug) {
      log("xwm: <- " + (EVENT_NAMES[code] || "event " + code) + (m.u8(0) & 0x80 ? " (sent)" : "") + " " + m.u32(4) + " " + m.u32(8)
          + (code === EV.ClientMessage ? " " + m.u32(12) + " " + m.u32(16) : ""));
    }
    if (ws.xselectionEvent && ws.xselectionEvent(m, code)) {
      return;
    }
    switch (code) {
    case EV.CreateNotify: {
      const parent = m.u32(4), id = m.u32(8);
      if (parent === root && !wins.has(id) && id !== check) {
        newWin(id, m.i16(12), m.i16(14), m.u16(16), m.u16(18), !!m.u8(22));
      }
      break;
    }
    case EV.DestroyNotify: {
      const w = wins.get(m.u32(8));
      if (w) {
        unpair(w);
        wins.delete(w.id);
        if (focusWin === w.id) {
          focusWin = 0;
        }
      }
      break;
    }
    case EV.MapRequest: {
      const id = m.u32(8);
      const w = wins.get(id);
      if (!w) {
        conn.mapWindow(id);	// (not a child of the root we know: as it asks)
        break;
      }
      readProps(w, PROPS, () => {
        if (wins.get(id) !== w) {
          return;
        }
        w.managed = true;
        w.iconic = false;
        conn.changeProperty(id, A.WM_STATE, A.WM_STATE, 32, [1, 0]);	// NormalState
        const decorated = w.motifDeco && !w.gtkExtents && !isPopup(w);
        conn.changeProperty(id, A._NET_FRAME_EXTENTS, A.CARDINAL, 32,
                            decorated ? [chrome.border, chrome.border, chrome.title, chrome.border] : [0, 0, 0, 0]);
        conn.mapWindow(id);
      });
      break;
    }
    case EV.MapNotify: {
      const w = wins.get(m.u32(8));
      if (w) {
        w.mapped = true;
        if (w.or && !w.surface) {
          readProps(w, ["WM_TRANSIENT_FOR", "_NET_WM_WINDOW_TYPE"], () => tryPair(w));
        } else {
          tryPair(w);
        }
      }
      break;
    }
    case EV.UnmapNotify: {
      const w = wins.get(m.u32(8));
      if (w) {
        w.mapped = false;
        unpair(w);
        if (w.managed) {
          w.managed = false;
          conn.deleteProperty(w.id, A.WM_STATE);	// withdrawn (ICCCM 4.1.4)
        }
      }
      break;
    }
    case EV.ConfigureNotify: {
      const w = wins.get(m.u32(8));
      if (w && m.u32(4) === root) {
        w.x = m.i16(16);
        w.y = m.i16(18);
        w.w = m.u16(20);
        w.h = m.u16(22);
        w.or = !!m.u8(26);
        if (w.surface) {
          w.surface.dirty = true;
          ws.seatChanged();
        }
      }
      break;
    }
    case EV.ConfigureRequest:
      configureRequest(m);
      break;
    case EV.PropertyNotify: {
      const w = wins.get(m.u32(4));
      const atom = m.u32(8);
      if (w) {
        const name = PROPS.find((n) => A[n] === atom);
        if (name && name !== "_NET_WM_STATE") {
          readProps(w, [name], () => applyProps(w));
        }
      }
      break;
    }
    case EV.ClientMessage:
      clientMessage(m);
      break;
    default:
      break;
    }
  };

  const configureRequest = (m) => {
    const id = m.u32(8), mask = m.u16(26);
    const req = { x: m.i16(16), y: m.i16(18), w: m.u16(20), h: m.u16(22), border: m.u16(24) };
    const w = wins.get(id);
    const s = w && w.surface;
    if (!s || !s.top) {
      // A window that is not shown yet (or is a popup): as it asks, borders aside (we draw the frames).
      const values = {};
      if (mask & X.CONFIG.X) {
        values.x = req.x;
      }
      if (mask & X.CONFIG.Y) {
        values.y = req.y;
      }
      if (mask & X.CONFIG.Width) {
        values.width = req.w;
      }
      if (mask & X.CONFIG.Height) {
        values.height = req.h;
      }
      if (mask & X.CONFIG.BorderWidth) {
        values.border = 0;
      }
      if (Object.keys(values).length) {
        conn.configureWindow(id, values);
      } else if (w) {
        syntheticConfigure(w);
      }
      return;
    }
    const t = s.top;
    if ((mask & (X.CONFIG.Width | X.CONFIG.Height)) && !(s.flags & (F.MAXIMIZED | F.FULLSCREEN))) {
      const e = extents(w);
      t.size = [(mask & X.CONFIG.Width ? req.w : w.w) - e[0] - e[1], (mask & X.CONFIG.Height ? req.h : w.h) - e[2] - e[3]];
    }
    if (!configure(s)) {
      syntheticConfigure(w);
    }
    s.dirty = true;
  };

  const clientMessage = (m) => {
    const id = m.u32(4), type = m.u32(8);
    const data = [m.u32(12), m.u32(16), m.u32(20), m.u32(24), m.u32(28)];
    const w = wins.get(id);
    const s = w && w.surface;
    if (type === A.WL_SURFACE_SERIAL) {
      if (w) {
        w.serial = data[1] * 4294967296 + data[0];
        tryPair(w);
      }
    } else if (type === A.WL_SURFACE_ID) {
      if (w) {
        w.wlId = data[0];
        tryPair(w);
      }
    } else if (type === A._NET_WM_STATE) {
      if (!w) {
        return;
      }
      // data: action (0 remove, 1 add, 2 toggle), two properties
      const action = data[0], props = [data[1], data[2]];
      const change = (has, now) => action === 1 ? true : action === 0 ? false : !now;
      if (props.includes(A._NET_WM_STATE_FULLSCREEN)) {
        const now = s ? !!(s.flags & F.FULLSCREEN) : w.netStateWanted.has(A._NET_WM_STATE_FULLSCREEN);
        const on = change(true, now);
        if (s && s.top) {
          ws.setFullscreen(s, on);
        } else if (on) {
          w.netStateWanted.add(A._NET_WM_STATE_FULLSCREEN);
        } else {
          w.netStateWanted.delete(A._NET_WM_STATE_FULLSCREEN);
        }
      }
      if (props.includes(A._NET_WM_STATE_MAXIMIZED_VERT) || props.includes(A._NET_WM_STATE_MAXIMIZED_HORZ)) {
        const now = s ? !!(s.flags & F.MAXIMIZED) : w.netStateWanted.has(A._NET_WM_STATE_MAXIMIZED_VERT);
        const on = change(true, now);
        if (s && s.top) {
          ws.setMaximized(s, on);
        } else {
          for (const a of [A._NET_WM_STATE_MAXIMIZED_VERT, A._NET_WM_STATE_MAXIMIZED_HORZ]) {
            if (on) {
              w.netStateWanted.add(a);
            } else {
              w.netStateWanted.delete(a);
            }
          }
        }
      }
      if (props.includes(A._NET_WM_STATE_HIDDEN) && s && s.top && change(true, !!(s.flags & F.MINIMIZED))) {
        ws.minimize(s);
      }
      // KWin's states: above, below, shaded, on all desktops, out of the task bar and pager, wanting attention
      if (s && s.top) {
        const t = s.top;
        const states = [[A._NET_WM_STATE_ABOVE, () => !!t.above, (on) => ws.setKeepAbove(s, on)],
                        [A._NET_WM_STATE_BELOW, () => !!t.below, (on) => ws.setKeepBelow(s, on)],
                        [A._NET_WM_STATE_SHADED, () => !!t.shaded, (on) => ws.setShaded(s, on)],
                        [A._NET_WM_STATE_STICKY, () => t.vd === null, (on) => ws.windowToDesktop(s, on ? -1 : ws.desktops.current)],
                        [A._NET_WM_STATE_SKIP_TASKBAR, () => !!t.skipTaskbar, (on) => { t.skipTaskbar = on; s.dirty = true; }],
                        [A._NET_WM_STATE_SKIP_PAGER, () => !!t.skipPager, (on) => { t.skipPager = on; s.dirty = true; }],
                        [A._NET_WM_STATE_DEMANDS_ATTENTION, () => !!t.attention, (on) => { t.attention = on; s.dirty = true; }]];
        for (const [atom, now, set] of states) {
          if (props.includes(atom)) {
            set(change(true, now()));
          }
        }
        netState(w, s);
      } else {
        for (const atom of [A._NET_WM_STATE_ABOVE, A._NET_WM_STATE_BELOW, A._NET_WM_STATE_SHADED, A._NET_WM_STATE_STICKY,
                            A._NET_WM_STATE_SKIP_TASKBAR, A._NET_WM_STATE_SKIP_PAGER]) {
          if (props.includes(atom)) {
            if (change(true, w.netStateWanted.has(atom))) {
              w.netStateWanted.add(atom);
            } else {
              w.netStateWanted.delete(atom);
            }
          }
        }
      }
    } else if (type === A._NET_CURRENT_DESKTOP) {
      if (ws.setCurrentDesktop) {
        ws.setCurrentDesktop(data[0]);
      }
    } else if (type === A._NET_NUMBER_OF_DESKTOPS) {
      if (ws.setDesktopCount && data[0] >= 1) {
        ws.setDesktopCount(data[0]);
      }
    } else if (type === A._NET_SHOWING_DESKTOP) {
      if (ws.setShowingDesktop) {
        ws.setShowingDesktop(data[0] !== 0);
      }
    } else if (type === A._NET_WM_DESKTOP) {
      if (s && s.top && ws.windowToDesktop) {
        ws.windowToDesktop(s, data[0] === 0xffffffff ? -1 : data[0]);
      } else if (w) {
        w.desktopWanted = data[0] >>> 0;
      }
    } else if (type === A._NET_ACTIVE_WINDOW) {
      if (s && s.top) {
        ws.activate(s);
      }
    } else if (type === A._NET_WM_MOVERESIZE) {
      // data: x, y (root), direction (0..7 the edges from the top left corner clockwise, 8 move, 11 cancel), button
      if (s && s.top && ws.startGrab) {
        const direction = data[2];
        const edges = [5, 1, 9, 8, 10, 2, 6, 4][direction];
        if (direction === 8) {
          ws.startGrab(s, "move", 0);
        } else if (edges) {
          ws.startGrab(s, "resize", edges);
        }
      }
    } else if (type === A.WM_CHANGE_STATE) {
      if (s && s.top && data[0] === 3) {	// IconicState
        ws.minimize(s);
      }
    } else if (type === A._NET_CLOSE_WINDOW) {
      if (s) {
        ws.xwmClose(s);
      }
    }
  };

  // ---- xwayland_shell_v1: Xwayland's half of the matching (only Xwayland is told of it)

  H.xwayland_shell_v1 = {
    destroy() {},
    get_xwayland_surface(shell, id, surfaceObject) {
      const s = surfaceObject.surface;
      if (s.role && s.role !== "xwayland") {
        ws.error(shell.client, shell, 0, "get_xwayland_surface: the surface has another role");
        return;
      }
      s.role = "xwayland";
      const object = ws.create(shell.client, id, "xwayland_surface_v1", 1);
      object.surface = s;
    },
  };

  H.xwayland_surface_v1 = {
    set_serial(object, lo, hi) {
      const s = object.surface;
      if (ws.xdebug) {
        log("xwm: surface " + s.id + " has serial " + lo + " (" + hi + ")");
      }
      const serial = hi * 4294967296 + lo;
      if (s.xserial !== undefined && serials.get(s.xserial) === s) {
        serials.delete(s.xserial);
      }
      s.xserial = serial;
      serials.set(serial, s);
      for (const w of wins.values()) {
        if (w.serial === serial) {
          tryPair(w);
        }
      }
    },
    destroy() {},
  };

  // ---- start

  const enabled = makeSocket();
  if (enabled) {
    global("xwayland_shell_v1", 1, () => {}, (client) => !!client.xwayland);
  }

  // From server.js once its epoll is there.
  ws.xwmStart = () => {
    if (enabled) {
      ws.watchFd(listener, onPending);
      log("xwm: X programs can connect to :" + DISPLAY + " (Xwayland starts when the first does)");
    }
  };

  // For tests: what the window manager knows.
  ws.xwmState = () => ({
    xwayland: xpid, windows: [...wins.values()].map((w) => ({ id: w.id, mapped: w.mapped, or: w.or, title: title(w),
      surface: w.surface ? w.surface.id : 0, x: w.x, y: w.y, w: w.w, h: w.h })),
    focus: focusWin,
  });
})
