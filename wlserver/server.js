// The server's loop: the listening socket, the clients, and a turn every
// few milliseconds.
//
// Waiting is epoll_pwait, which blocks this Worker: no task of the browser's
// runs meanwhile, and nothing from outside can wake us but a descriptor. The
// page has one: its doorbell (the kernel's "Wasm host" input device, which
// seat.js watches), rung whenever it has written something into the table
// for us. With it a turn waits up to 50 ms (for what is timed here: the
// heartbeat, a drop's wait); without it (an older kernel) never more than
// 8 ms (the page's frames come every 16). A turn looks at the table on its
// way out (compositor.js: pump), and then yields to the browser once (a
// message to ourselves brings the next turn), so that what the browser has
// for this Worker gets done between turns.
(function (ws) {
  "use strict";

  const { sys1, sys2, sys3, sys4, sys6, SYS, alloc, dv, u8, path, P0, log, envp } = ws;
  const AF_UNIX = 1, SOCK_STREAM = 1, SOCK_NONBLOCK = 0o4000, SOCK_CLOEXEC = 0o2000000;
  const O_CLOEXEC = 0o2000000;
  const AT_FDCWD = -100;
  const EPOLL_CTL_ADD = 1, EPOLL_CTL_MOD = 3, EPOLL_CTL_DEL = 2;
  const EPOLLIN = 1, EPOLLOUT = 4, EPOLLERR = 8, EPOLLHUP = 16;
  const EINTR = 4, EAGAIN = 11;
  const MAX_EVENTS = 64;
  const TURN_MS = 8, TURN_MS_DOORBELL = 50;

  const env = (name) => {
    const prefix = name + "=";
    const e = envp.find((e) => e.startsWith(prefix));
    return e ? e.slice(prefix.length) : null;
  };

  // The socket, where clients look: $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY.
  const dir = env("XDG_RUNTIME_DIR");
  if (!dir) {
    log("wlserver: XDG_RUNTIME_DIR is not set");
    ws.exit(1);
  }
  const socketPath = dir + "/" + (env("WAYLAND_DISPLAY") || "wayland-0");
  if (socketPath.length > 107) {
    log("wlserver: the socket's path is too long: " + socketPath);
    ws.exit(1);
  }
  // The lock next to the socket, held while we run: how a second server (or `desktop wayland`) knows that one
  // runs already (libwayland's servers do the same).
  {
    const O_RDWR = 2, O_CREAT = 0o100, LOCK_EX = 2, LOCK_NB = 4;
    const lock = sys4(SYS.openat, AT_FDCWD, path(socketPath + ".lock", P0), O_RDWR | O_CREAT | O_CLOEXEC, 0o660);
    if (lock < 0) {
      log("wlserver: cannot open " + socketPath + ".lock (error " + -lock + ")");
      ws.exit(1);
    }
    const r = sys2(SYS.flock, lock, LOCK_EX | LOCK_NB);
    if (r === -11) {
      log("wlserver: another server holds " + socketPath + ".lock: not starting a second one");
      ws.exit(1);
    }
    if (r < 0) {
      log("wlserver: flock " + socketPath + ".lock: error " + -r + " (going on without the lock)");
    }
  }
  sys3(SYS.unlinkat, AT_FDCWD, path(socketPath, P0), 0);
  const listener = sys3(SYS.socket, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    log("wlserver: socket: error " + -listener);
    ws.exit(1);
  }
  const addr = alloc(112);
  {
    const m = u8();
    m.fill(0, addr, addr + 112);
    dv().setUint16(addr, AF_UNIX, true);
    const raw = new TextEncoder().encode(socketPath);
    m.set(raw, addr + 2);
    const r = sys3(SYS.bind, listener, addr, 2 + raw.length + 1);
    if (r < 0) {
      log("wlserver: bind " + socketPath + ": error " + -r);
      ws.exit(1);
    }
    const l = sys2(SYS.listen, listener, 128);
    if (l < 0) {
      log("wlserver: listen: error " + -l);
      ws.exit(1);
    }
  }

  const epfd = sys1(SYS.epoll_create1, O_CLOEXEC);
  const events = alloc(16 * MAX_EVENTS);
  const one = alloc(16);
  const watch = (op, fd, mask) => {
    const v = dv();
    v.setUint32(one, mask, true);
    v.setUint32(one + 4, 0, true);
    v.setBigUint64(one + 8, BigInt(fd), true);
    return sys4(SYS.epoll_ctl, epfd, op, fd, one);
  };
  watch(EPOLL_CTL_ADD, listener, EPOLLIN);

  // Other descriptors to wait on (the input devices, a pipe the clipboard comes through), with what to do.
  const handlers = new Map();
  ws.watchFd = (fd, handler) => {
    handlers.set(fd, handler);
    watch(EPOLL_CTL_ADD, fd, EPOLLIN);
  };
  ws.unwatchFd = (fd) => {
    if (handlers.delete(fd)) {
      watch(EPOLL_CTL_DEL, fd, 0);
    }
  };

  const clients = new Map();	// by descriptor
  ws.clients = () => clients.values();

  const accept = () => {
    for (;;) {
      const fd = sys4(SYS.accept4, listener, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd === -EINTR) {
        continue;
      }
      if (fd < 0) {
        if (fd !== -EAGAIN) {
          log("wlserver: accept: error " + -fd);
        }
        return;
      }
      ws.addClient(fd);
    }
  };

  // A client on a connected socket (one accepted here, or our end of a socket pair whose other end a program
  // we started has: Xwayland, xwm.js). Returns the client.
  ws.addClient = (fd) => {
    const client = {
      fd, objects: new Map(), fds: [], out: new ws.Outgoing(), gone: false,
      inBytes: new Uint8Array(16384), inView: null, inLength: 0, writing: false,
      wantWrite(yes) {
        if (yes !== client.writing && !client.gone) {
          client.writing = yes;
          watch(EPOLL_CTL_MOD, fd, EPOLLIN | (yes ? EPOLLOUT : 0));
        }
      },
    };
    client.inView = new DataView(client.inBytes.buffer);
    client.display = ws.create(client, 1, "wl_display", 1);
    clients.set(fd, client);
    watch(EPOLL_CTL_ADD, fd, EPOLLIN);
    ws.stats.clients++;
    if (ws.debug) {
      log("wlserver: client " + fd + " connected");
    }
    return client;
  };

  ws.disconnect = (client, why) => {
    if (client.gone) {
      return;
    }
    client.gone = true;
    if (ws.debug || !/^closed/.test(why)) {
      log("wlserver: client " + client.fd + " gone (" + why + ")");
    }
    watch(EPOLL_CTL_DEL, client.fd, 0);
    clients.delete(client.fd);
    sys1(SYS.close, client.fd);
    if (ws.dragClientGone) {
      ws.dragClientGone(client);
    }
    ws.dropOutgoing(client);
    // Its objects, surfaces first (a buffer's going must not find its surface gone... either order works,
    // but surfaces leave the table, which the page should see before their pools are unmapped).
    const objects = [...client.objects.values()];
    for (const o of objects) {
      if (o.interface === "wl_surface") {
        ws.destroy(o);
      }
    }
    for (const o of objects) {
      ws.destroy(o);
    }
    for (const fd of client.fds) {
      sys1(SYS.close, fd);
    }
    client.fds = [];
    ws.stats.clients--;
  };

  // The screen's size, for programs that size themselves by it before they connect (`desktop wayland-x` makes
  // Xwayland's screen fit it): "width height scale panel title border" in a file next to the socket, written
  // once the page has said what its screen is (or two seconds after we started, without a page), and again
  // whenever it changes.
  const screenFile = dir + "/wlserver.screen";
  sys3(SYS.unlinkat, AT_FDCWD, path(screenFile, P0), 0);
  let screenWritten = false;
  const startedAt = performance.now();
  const writeScreen = () => {
    const O_WRONLY = 1, O_CREAT = 0o100, O_TRUNC = 0o1000;
    const s = ws.screen;
    const fd = sys4(SYS.openat, AT_FDCWD, path(screenFile, P0), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644);
    if (fd >= 0) {
      ws.writeAll(fd, [s.width, s.height, s.scale, s.panel, ws.chrome.title, ws.chrome.border].join(" ") + "\n");
      sys1(SYS.close, fd);
    }
    screenWritten = true;
  };

  // ---- turns

  // When the program it was started for has ended, `desktop wayland` leaves a file, and the server goes once it
  // has had no clients for ten seconds (programs like Kate go into the background at once, and connect later).
  const doneFile = dir + "/wlserver.done";
  let lonelySince = performance.now();
  const lingerCheck = (t) => {
    if (ws.stats.clients > 0) {
      lonelySince = t;
      return;
    }
    if (t - lonelySince > 10000 && ws.readFile(doneFile) !== null) {
      log("wlserver: the program has ended and no client is left: done");
      ws.exit(0);
    }
  };

  const stats = ws.stats;
  let lastAnnounce = 0, lastStats = performance.now(), turns = 0, busyMs = 0;
  const statsOn = env("WLSERVER_STATS") === "1";
  const channel = new MessageChannel();
  let turning = false;

  const turn = () => {
    // (While the page is hidden the server's own turns are its clock for the frames it stands in for: compositor.js)
    const hidden = ws.table.pageHidden();
    const wait = ws.doorbell ? TURN_MS_DOORBELL : TURN_MS;
    const n = sys6(SYS.epoll_pwait, epfd, events, MAX_EVENTS, hidden ? Math.min(wait, Math.ceil(1000 / hidden)) : wait, 0, 0);
    const began = performance.now();
    if (n < 0 && n !== -EINTR) {
      throw new ws.SystemError(-n, "epoll_pwait");
    }
    const v = dv();
    for (let i = 0; i < n; i++) {
      const ready = v.getUint32(events + 16 * i, true);
      const fd = Number(v.getBigUint64(events + 16 * i + 8, true));
      if (fd === listener) {
        accept();
        continue;
      }
      const handler = handlers.get(fd);
      if (handler) {
        handler(ready);
        continue;
      }
      const client = clients.get(fd);
      if (!client) {
        continue;
      }
      if (ready & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
        if (!ws.receive(client)) {
          continue;
        }
      }
      if (ready & EPOLLOUT) {
        ws.flush(client);
      }
    }
    const out = ws.table.output();
    if (out) {
      ws.setOutput(out.width, out.height, out.scale, out.panel, out.struts);
      ws.table.outputDone(out.serial);
      writeScreen();
    }
    ws.orders();
    ws.keyboardTurn();
    ws.dataTurn();
    if (ws.textInputTurn) {
      ws.textInputTurn();
    }
    ws.dragTurn();
    ws.seatTurn();
    ws.pump();
    for (const client of clients.values()) {
      ws.flush(client);
    }
    ws.table.heartbeat();
    const t = performance.now();
    if (t - lastAnnounce > 1000) {
      lastAnnounce = t;
      ws.table.announce();
      ws.reap();
      lingerCheck(t);
      if (!screenWritten && t - startedAt > 2000) {
        writeScreen();
      }
    }
    turns++;
    busyMs += t - began;
    if (statsOn && t - lastStats > 5000) {
      const dt = (t - lastStats) / 1000;
      log("stats: " + (turns / dt).toFixed(0) + " turns/s, busy " + (busyMs / dt).toFixed(1) + " ms/s, "
          + (stats.presents / dt).toFixed(1) + " presents/s, " + (stats.damageBytes / dt / 1024).toFixed(0)
          + " KiB/s damage, " + stats.clients + " clients, " + ws.windows.length + " windows, "
          + stats.pools + " pools, " + stats.resizes + " resizes"
          + (typeof performance !== "undefined" && performance.memory ? ", JS heap " + (performance.memory.usedJSHeapSize >> 20) + " MiB"
            : typeof process !== "undefined" && process.memoryUsage ? ", JS heap " + (process.memoryUsage().heapUsed >> 20) + " MiB" : ""));
      lastStats = t;
      turns = 0;
      busyMs = 0;
      stats.presents = 0;
      stats.damageBytes = 0;
    }
  };

  channel.port1.onmessage = () => {
    turning = false;
    try {
      turn();
    } catch (error) {
      if (error && error.name === "Trap") {
        ws.host.error(error);
        return;
      }
      ws.fatal(error);
      return;
    }
    again();
  };
  const again = () => {
    if (!turning) {
      turning = true;
      channel.port2.postMessage(0);
    }
  };

  ws.seatStart();
  if (ws.xwmStart) {
    ws.xwmStart();
  }
  log("wlserver: listening on " + socketPath);
  ws.table.announce();
  again();
})
