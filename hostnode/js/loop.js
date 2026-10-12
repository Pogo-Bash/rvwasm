// The event loop.
//
// Node's library is written against libuv's loop: timers, then waiting for
// descriptors, then immediates, each callback followed by the process.nextTick
// queue and the jobs of promises. Two things here are not as in Node.
//
// Waiting is a system call of the kernel (epoll), and it blocks this Worker,
// which is what a process with nothing to do should be doing. But jobs of
// promises are run by the browser only when no JavaScript is on the stack,
// and there is no way to ask for them. So every callback into the program is
// a task of its own for the browser's loop (a message to ourselves), and the
// jobs run when it returns: that is one `turn` below. Ticks are run by us at
// the end of the callback, before the jobs, as Node does.
//
// And what the browser itself does later (an answer to fetch, a key derived
// by its cryptography) arrives as tasks of the browser's loop, which do not
// run while we sit in the kernel. While such operations are open
// (loop.hostRef), the loop does not block there: it looks at the descriptors
// without waiting and comes back by a timer of the browser.
(function (hn) {
  "use strict";

  const { sys1, sys3, sys4, sys6, SYS, alloc, dv, web } = hn;
  const now0 = web.performance.now();
  const performance = web.performance;
  const O_CLOEXEC = 0o2000000;
  const EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3;
  const EINTR = 4;
  const MAX_EVENTS = 64;

  const epfd = sys1(SYS.epoll_create1, O_CLOEXEC);
  const events = alloc(16 * MAX_EVENTS);
  const one = alloc(16);

  // Descriptors watched: the descriptor's number is what the kernel hands back.
  const watchers = new Map();

  // Callbacks waiting for their turn.
  let jobs = [];
  let jobAt = 0;

  const loop = {
    // Handles and requests that keep the program going.
    refs: 0,
    // Operations of the browser that are open.
    hostOps: 0,
    // Set by node.js: run a callback of the program, with what Node does around one.
    call: null,
    // Set by node.js: are there ticks to run (asked for by jobs of promises since the last callback)?
    ticksPending: () => false,
    runTicks: () => {},
    // Set by node.js: nothing keeps the program going. Returns after `beforeExit` if something does again.
    finish: null,
    // Set by the timers binding. The one timer of Node's timer lists, and its immediates.
    timerCallback: null,
    timerDue: -1,
    timerRef: true,
    immediateCallback: null,
    immediateInfo: null,
    // By signal number: sets of functions.
    signals: new Map(),
    started: false,

    // Milliseconds since the loop was made.
    now: () => performance.now() - now0,
    hrtime: () => performance.now(),

    // Have fn called on a turn of its own.
    enqueue(fn) {
      jobs.push(fn);
      kick();
    },

    ref() {
      loop.refs++;
    },
    unref() {
      loop.refs--;
    },
    hostRef() {
      loop.hostOps++;
    },
    hostUnref() {
      loop.hostOps--;
      kick();
    },

    // Watch a descriptor: callback(events) on a turn when it has any of `mask` (EPOLLIN 1, EPOLLOUT 4;
    // errors and hang-ups always). Returns the watcher: set(mask) changes what is asked for (0: nothing,
    // while staying known), close() ends it.
    watch(fd, mask, callback) {
      const watcher = {
        fd, mask: 0, callback, registered: false,
        set(m) {
          if (m === watcher.mask) {
            return 0;
          }
          let r = 0;
          const v = dv();
          v.setUint32(one, m, true);
          v.setUint32(one + 4, 0, true);
          v.setBigUint64(one + 8, BigInt(fd), true);
          if (m === 0) {
            if (watcher.registered) {
              r = sys4(SYS.epoll_ctl, epfd, EPOLL_CTL_DEL, fd, one);
              watcher.registered = false;
            }
          } else if (watcher.registered) {
            r = sys4(SYS.epoll_ctl, epfd, EPOLL_CTL_MOD, fd, one);
          } else {
            r = sys4(SYS.epoll_ctl, epfd, EPOLL_CTL_ADD, fd, one);
            watcher.registered = r === 0;
          }
          if (r === 0) {
            watcher.mask = m;
          }
          return r;
        },
        close() {
          watcher.set(0);
          if (watchers.get(fd) === watcher) {
            watchers.delete(fd);
          }
          watcher.callback = null;
        },
      };
      watchers.set(fd, watcher);
      const r = watcher.set(mask);
      if (r < 0) {
        watchers.delete(fd);
        watcher.error = r;
      }
      return watcher;
    },

    // Signals. The ones the program listens for are blocked, so that they wait in the kernel, and are read
    // from a descriptor when the loop gets to it (see hostnode.c on why not handlers). on: the first
    // listener for a signal has come; off: the last has gone.
    signalOn(signal) {
      setSignal(signal, true);
    },
    signalOff(signal) {
      // What has arrived and not been read is the old listener's, not the default action's.
      readSignals(true);
      setSignal(signal, false);
    },

    alive() {
      return loop.refs > 0 || loop.hostOps > 0 || jobAt < jobs.length ||
        (loop.timerDue >= 0 && loop.timerRef) ||
        (loop.immediateInfo !== null && loop.immediateInfo[1] > 0);	// kRefCount
    },

    start() {
      loop.started = true;
      kick();
    },
  };

  const SIG_BLOCK = 0, SIG_UNBLOCK = 1;
  const SFD = 0o2000000 | 0o4000;	// close on exec, non-blocking
  let signalFd = -1;
  let signalMask = 0n;
  const maskAt = alloc(8);
  const setSignal = (signal, on) => {
    const bit = 1n << BigInt(signal - 1);
    signalMask = on ? signalMask | bit : signalMask & ~bit;
    const v = dv();
    v.setBigUint64(maskAt, signalMask, true);
    const fd = sys4(SYS.signalfd4, signalFd, maskAt, 8, SFD);
    if (fd < 0) {
      throw new hn.SystemError(-fd, "signalfd");
    }
    if (signalFd < 0) {
      signalFd = fd;
      loop.watch(fd, 1, () => readSignals(false));
    }
    v.setBigUint64(maskAt, bit, true);
    sys4(SYS.rt_sigprocmask, on ? SIG_BLOCK : SIG_UNBLOCK, maskAt, 0, 8);
  };
  const readSignals = (discard) => {
    if (signalFd < 0) {
      return;
    }
    for (;;) {
      const n = sys3(SYS.read, signalFd, events, 128 * 8);	// (the events block is free outside poll)
      if (n < 128) {
        return;
      }
      for (let at = events; at < events + n; at += 128) {
        const signal = dv().getUint32(at, true);
        const handlers = loop.signals.get(signal);
        if (handlers && !discard) {
          for (const handler of [...handlers]) {
            jobs.push(() => handler(signal));
          }
        }
      }
    }
  };

  // Turns. At most one is on its way at any time; `ticket` tells a turn that was overtaken (one that a
  // timer of the browser brings, after a message has brought another) that it is not wanted any more.
  const channel = new web.MessageChannel();
  let scheduled = false;
  let ticket = 0;
  channel.port1.onmessage = (event) => {
    if (event.data === ticket) {
      turn();
    }
  };
  const again = () => {
    scheduled = true;
    channel.port2.postMessage(++ticket);
  };
  const later = (ms) => {
    scheduled = true;
    const mine = ++ticket;
    web.setTimeout(() => {
      if (mine === ticket) {
        turn();
      }
    }, ms);
  };
  let waitingForHost = false;
  const kick = () => {
    if (loop.started && (!scheduled || waitingForHost)) {
      waitingForHost = false;
      again();
    }
  };

  // Which part of an iteration of the loop is next: 0 timers, 1 descriptors, 2 immediates and the end.
  let phase = 0;
  // Has the browser had a turn of its own since the program last ran? What it has to tell after a callback
  // (that a promise was rejected with nobody listening) it tells in a task, which waits behind the turn
  // that is already on its way. So before sleeping in the kernel or ending the program, one empty turn.
  let quiet = false;

  const poll = (timeout) => {
    const n = sys6(SYS.epoll_pwait, epfd, events, MAX_EVENTS, timeout, 0, 0);
    if (n < 0) {
      if (n !== -EINTR) {
        throw new hn.SystemError(-n, "epoll_pwait");
      }
      return;
    }
    const v = dv();
    for (let i = 0; i < n; i++) {
      const ready = v.getUint32(events + 16 * i, true);
      const fd = Number(v.getBigUint64(events + 16 * i + 8, true));
      const watcher = watchers.get(fd);
      if (watcher && watcher.callback) {
        jobs.push(() => {
          if (watcher.callback) {
            watcher.callback(ready);
          }
        });
      }
    }
  };

  const step = () => {
    // Ticks asked for by the jobs of promises that ran after the last callback come before anything else.
    if (loop.ticksPending()) {
      quiet = false;
      loop.runTicks();
      return again();
    }
    if (jobAt < jobs.length) {
      const job = jobs[jobAt++];
      if (jobAt === jobs.length) {
        jobs = [];
        jobAt = 0;
      }
      quiet = false;
      loop.call(job);
      return again();
    }
    if (phase === 0) {
      phase = 1;
      if (loop.timerDue >= 0 && loop.timerDue <= loop.now()) {
        loop.timerDue = -1;
        quiet = false;
        loop.call(loop.timerCallback);
        return again();
      }
    }
    if (phase === 1) {
      // How long to wait: not at all if there is something to do right after, or nothing to wait for.
      let timeout = -1;
      const immediates = loop.immediateInfo !== null && loop.immediateInfo[1] > 0;
      if (immediates || !loop.alive()) {
        timeout = 0;
      } else if (loop.timerDue >= 0) {
        timeout = Math.max(0, Math.ceil(loop.timerDue - loop.now()));
      }
      if (timeout !== 0 && !quiet) {
        quiet = true;
        return again();
      }
      phase = 2;
      if (loop.hostOps > 0 && timeout !== 0) {
        // The browser has something open for us: it cannot tell us while we wait in the kernel.
        poll(0);
          if (jobAt === jobs.length && !(loop.timerDue >= 0 && loop.timerDue <= loop.now())) {
          phase = 1;
          waitingForHost = true;
          return later(timeout < 0 ? 4 : Math.min(timeout, 4));
        }
      } else {
        poll(timeout);
        }
      if (jobAt < jobs.length) {
        return again();
      }
    }
    phase = 0;
    if (loop.immediateInfo !== null && loop.immediateInfo[0] > 0) {	// kCount
      quiet = false;
      loop.call(loop.immediateCallback);
      return again();
    }
    if (!loop.alive()) {
      if (!quiet) {
        quiet = true;
        return again();
      }
      // Returns only if the program found something to do after all.
      quiet = false;
      loop.finish();
    }
    return again();
  };

  const turn = () => {
    scheduled = false;
    waitingForHost = false;
    try {
      step();
    } catch (error) {
      if (error && error.name === "Trap") {
        hn.host.error(error);
        return;
      }
      hn.fatal(error);
    }
  };

  hn.loop = loop;
})
