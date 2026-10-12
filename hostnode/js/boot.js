// hostnode: Node.js on the JavaScript engine the machine itself runs in.
//
// This text is what /bin/hostnode (hostnode.c) hands to the host: an
// expression for a function, called once with what a process on Linux/Wasm
// is made of (tools/patch-runtime.py, item 7). From here on this is the
// program of the task. There is no Wasm under it and no engine inside it:
// JavaScript of a Node.js program is run by the engine of the browser, and
// what Node.js gets from its C++ half, it gets here from system calls of the
// machine's kernel.
//
// The parts, all in the directory this file was read from:
//   boot.js        this: memory, system calls, files, loading the other parts
//   loop.js        the event loop: epoll, timers, signals, turns
//   node.js        starting Node's own library (lib/), which is Node's, unchanged
//   binding/*.js   what Node's library asks of internalBinding(), on system calls
//   lib/**         Node's library, taken out of the Node.js of the guest system
(function (host) {
  "use strict";

  const memory = host.memory;
  const C = host.instance.exports;	// the C library under us (hostnode.c exports everything)
  const K = host.syscalls;

  // The stack and thread pointers the kernel wants with every system call: those of the Wasm program, as it
  // left them. A signal handler runs on that stack, below this.
  const SP = host.stack_pointer.value;
  const TP = C.__get_tls_base();

  const B = BigInt, N = Number;
  // System calls by the number of their arguments. Arguments are integers (addresses too: the machine has
  // 4 GiB at most); the result is a number, negative for an error.
  let sys0 = (n) => N(K[0](SP, TP, B(n)));
  let sys1 = (n, a) => N(K[1](SP, TP, B(n), B(a)));
  let sys2 = (n, a, b) => N(K[2](SP, TP, B(n), B(a), B(b)));
  let sys3 = (n, a, b, c) => N(K[3](SP, TP, B(n), B(a), B(b), B(c)));
  let sys4 = (n, a, b, c, d) => N(K[4](SP, TP, B(n), B(a), B(b), B(c), B(d)));
  let sys5 = (n, a, b, c, d, e) => N(K[5](SP, TP, B(n), B(a), B(b), B(c), B(d), B(e)));
  let sys6 = (n, a, b, c, d, e, f) => N(K[6](SP, TP, B(n), B(a), B(b), B(c), B(d), B(e), B(f)));

  // The kernel's numbers (include/uapi/asm-generic/unistd.h: this architecture has the generic table).
  const SYS = {
    getcwd: 17, epoll_create1: 20, epoll_ctl: 21, epoll_pwait: 22, dup: 23, dup3: 24, fcntl: 25,
    inotify_init1: 26, inotify_add_watch: 27, inotify_rm_watch: 28, ioctl: 29,
    flock: 32, mknodat: 33, mkdirat: 34, unlinkat: 35, symlinkat: 36, linkat: 37, statfs: 43, fstatfs: 44,
    truncate: 45, ftruncate: 46, fallocate: 47, faccessat: 48, chdir: 49, fchdir: 50, fchmod: 52, fchmodat: 53,
    fchownat: 54, fchown: 55, openat: 56, close: 57, pipe2: 59, getdents64: 61, lseek: 62, read: 63, write: 64,
    readv: 65, writev: 66, pread64: 67, pwrite64: 68, preadv: 69, pwritev: 70, sendfile: 71, ppoll: 73,
    signalfd4: 74, readlinkat: 78, fsync: 82, fdatasync: 83, utimensat: 88, exit: 93, exit_group: 94, waitid: 95,
    nanosleep: 101, clock_gettime: 113, sched_getaffinity: 123, sched_yield: 124, kill: 129, tkill: 130,
    timerfd_create: 85, timerfd_settime: 86, rt_sigaction: 134, rt_sigprocmask: 135, setpriority: 140, getpriority: 141, setregid: 143, setgid: 144,
    setreuid: 145, setuid: 146, getresuid: 148, getresgid: 150, times: 153, setpgid: 154, getpgid: 155,
    getsid: 156, setsid: 157, getgroups: 158, setgroups: 159, uname: 160, getrlimit: 163, getrusage: 165,
    umask: 166, prctl: 167, gettimeofday: 169, getpid: 172, getppid: 173, getuid: 174, geteuid: 175,
    getgid: 176, getegid: 177, gettid: 178, sysinfo: 179, socket: 198, socketpair: 199, bind: 200, listen: 201,
    accept: 202, connect: 203, getsockname: 204, getpeername: 205, sendto: 206, recvfrom: 207, setsockopt: 208,
    getsockopt: 209, shutdown: 210, sendmsg: 211, recvmsg: 212, munmap: 215, execve: 221, mmap: 222,
    accept4: 242, wait4: 260, prlimit64: 261, renameat2: 276, getrandom: 278, statx: 291, pidfd_open: 434,
    close_range: 436, faccessat2: 439,
  };

  // HOSTNODE_STRACE=1 in the environment: every system call on standard error, as RV_STRACE does for the
  // emulator (the environment is read further down; this looks at the raw block, once).
  {
    const m8 = new Uint8Array(memory.buffer);
    const d8 = new DataView(memory.buffer);
    const envAt = N(d8.getBigUint64(N(host.arg) + 16, true));
    let traced = false;
    for (let at = envAt; ; at += 8) {
      const p = N(d8.getBigUint64(at, true));
      if (!p) {
        break;
      }
      let s = "";
      for (let i = p; m8[i] && i < p + 24; i++) {
        s += String.fromCharCode(m8[i]);
      }
      if (s === "HOSTNODE_STRACE=1") {
        traced = true;
      }
    }
    if (traced) {
      const names = {};
      for (const name of Object.keys(SYS)) {
        names[SYS[name]] = name;
      }
      const enc = new TextEncoder();
      const at = N(C.malloc(4096n));
      const now = performance.now.bind(performance);	// (the program replaces the global one)
      let depth = 0;
      const say = (line) => {
        const raw = enc.encode(line.slice(0, 4000) + "\n");
        new Uint8Array(memory.buffer).set(raw, at);
        K[3](SP, TP, 64n, 2n, B(at), B(raw.length));
      };
      const shown = (n, args) => args.map((a, i) => {
        // The path argument of the calls that have one, as text.
        if (i === 1 && [SYS.openat, SYS.statx, SYS.readlinkat, SYS.faccessat, SYS.faccessat2, SYS.mkdirat,
                        SYS.unlinkat, SYS.renameat2, SYS.utimensat, SYS.fchmodat, SYS.fchownat].includes(n)) {
          const mem = new Uint8Array(memory.buffer);
          let t = "";
          for (let i = a; mem[i] && i < a + 200; i++) {
            t += String.fromCharCode(mem[i]);
          }
          return JSON.stringify(t);
        }
        return a;
      });
      // The call is said before it is made (one that never returns is then seen) and its result after.
      const wrap = (k, count) => (n, ...args) => {
        if (depth > 0 || n === 64 && args[0] === 2) {
          return N(K[count](SP, TP, B(n), ...args.map(B)));
        }
        depth++;
        const began = now();
        const call = (names[n] || n) + "(" + shown(n, args).join(", ") + ")";
        say("[strace] " + call + " ...");
        let r;
        try {
          r = N(K[count](SP, TP, B(n), ...args.map(B)));
        } finally {
          depth--;
        }
        say("[strace] " + call + " = " + r + (now() - began > 2 ? " (" + Math.round(now() - began) + " ms)" : ""));
        return r;
      };
      sys0 = wrap(0, 0);
      sys1 = wrap(1, 1);
      sys2 = wrap(2, 2);
      sys3 = wrap(3, 3);
      sys4 = wrap(4, 4);
      sys5 = wrap(5, 5);
      sys6 = wrap(6, 6);
    }
  }

  // The machine's memory. Its buffer is another object after the memory has grown, so the views are made
  // again when that has happened.
  let buffer = memory.buffer;
  let bytes = new Uint8Array(buffer);
  let view = new DataView(buffer);
  const renew = () => {
    buffer = memory.buffer;
    bytes = new Uint8Array(buffer);
    view = new DataView(buffer);
  };
  const u8 = () => {
    if (memory.buffer !== buffer) {
      renew();
    }
    return bytes;
  };
  const dv = () => {
    if (memory.buffer !== buffer) {
      renew();
    }
    return view;
  };

  const alloc = (size) => {
    const address = N(C.malloc(B(size)));
    if (!address) {
      throw new RangeError("hostnode: out of memory (" + size + " bytes)");
    }
    return address;
  };
  const free = (address) => C.free(B(address));

  // Text. (Decoders of the browser do not take shared memory: hence the copies.)
  const decoder = new TextDecoder("utf-8");
  const encoder = new TextEncoder();
  const text = (address, length) => {
    const m = u8();
    let ascii = true;
    for (let i = address; i < address + length; i++) {
      if (m[i] > 127) {
        ascii = false;
        break;
      }
    }
    if (ascii && length < 256) {
      let s = "";
      for (let i = address; i < address + length; i++) {
        s += String.fromCharCode(m[i]);
      }
      return s;
    }
    return decoder.decode(m.slice(address, address + length));
  };
  const cstring = (address) => {
    const m = u8();
    let end = address;
    while (m[end]) {
      end++;
    }
    return text(address, end - address);
  };
  // A copy of memory, as bytes of our own.
  const copy = (address, length) => u8().slice(address, address + length);

  // Places for the arguments of system calls: two paths, and a block for everything else. Nothing here is
  // used across a call into Node's library, so one of each is enough.
  const PATH_MAX = 4096;
  const SCRATCH = 65536;
  const P0 = alloc(2 * PATH_MAX + SCRATCH);
  const P1 = P0 + PATH_MAX;
  const S = P1 + PATH_MAX;

  class SystemError extends Error {
    constructor(errno, what) {
      super(what + ": error " + errno);
      this.errno = errno;
    }
  }

  // A path (a string, or bytes) as a C string at P0 or P1.
  const path = (p, where) => {
    const m = u8();
    if (typeof p === "string") {
      const n = p.length;
      if (n < PATH_MAX) {
        let i = 0;
        for (; i < n; i++) {
          const c = p.charCodeAt(i);
          if (c > 127 || c === 0) {
            break;
          }
          m[where + i] = c;
        }
        if (i === n) {
          m[where + n] = 0;
          return where;
        }
      }
      p = encoder.encode(p);
    }
    if (p.length >= PATH_MAX) {
      return -36;	// ENAMETOOLONG: the caller passes a negative address on, and the kernel says EFAULT
    }
    m.set(p, where);
    m[where + p.length] = 0;
    return where;
  };
  // Any text as a C string in memory of its own (to be freed).
  const cstringNew = (s) => {
    const encoded = typeof s === "string" ? encoder.encode(s) : s;
    const address = alloc(encoded.length + 1);
    const m = u8();
    m.set(encoded, address);
    m[address + encoded.length] = 0;
    return address;
  };

  const O_RDONLY = 0, O_CLOEXEC = 0o2000000;
  const AT_FDCWD = -100;
  const EINTR = 4, EAGAIN = 11;

  // A whole file, as bytes; null if it cannot be read.
  const readFile = (name) => {
    const fd = sys4(SYS.openat, AT_FDCWD, path(name, P0), O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
      return null;
    }
    const chunks = [];
    let total = 0;
    for (;;) {
      const n = sys3(SYS.read, fd, S, SCRATCH);
      if (n === -EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      chunks.push(copy(S, n));
      total += n;
    }
    sys1(SYS.close, fd);
    if (chunks.length === 1) {
      return chunks[0];
    }
    const all = new Uint8Array(total);
    let at = 0;
    for (const chunk of chunks) {
      all.set(chunk, at);
      at += chunk.length;
    }
    return all;
  };
  const readText = (name) => {
    const data = readFile(name);
    return data === null ? null : decoder.decode(data);
  };

  // Write bytes (or text) to a descriptor, all of them, waiting if need be.
  const writeAll = (fd, data) => {
    if (typeof data === "string") {
      data = encoder.encode(data);
    }
    let done = 0;
    while (done < data.length) {
      const part = Math.min(data.length - done, SCRATCH);
      u8().set(data.subarray(done, done + part), S);
      const n = sys3(SYS.write, fd, S, part);
      if (n === -EINTR) {
        continue;
      }
      if (n === -EAGAIN) {
        // Not ours to make blocking (someone else has it open too): wait for room.
        const v = dv();
        v.setInt32(S, fd, true);
        v.setInt16(S + 4, 4, true);	// POLLOUT
        v.setInt16(S + 6, 0, true);
        sys5(SYS.ppoll, S, 1, 0, 0, 0);
        continue;
      }
      if (n < 0) {
        return n;
      }
      done += n;
    }
    return done;
  };

  // What has to be put back before the program is gone (binding/stream.js: the terminal's settings).
  const atExit = [];
  const exit = (code) => {
    for (const f of atExit.splice(0)) {
      try {
        f();
      } catch (error) {
      }
    }
    sys1(SYS.exit_group, code & 0xff);
    // (Not reached: the kernel does not come back, and the Worker is ended from outside.)
    for (;;) {
      sys1(SYS.exit, code & 0xff);
    }
  };

  // What the Wasm program told us.
  const start = N(host.arg);
  const pointers = (address) => {
    const list = [];
    for (let at = address; ; at += 8) {
      const p = N(dv().getBigUint64(at, true));
      if (!p) {
        return list;
      }
      list.push(cstring(p));
    }
  };
  const argv = pointers(N(dv().getBigUint64(start + 8, true)));
  const envp = pointers(N(dv().getBigUint64(start + 16, true)));
  const home = cstring(N(dv().getBigUint64(start + 24, true)));

  const hn = {
    host, memory, C, SYS, sys0, sys1, sys2, sys3, sys4, sys5, sys6,
    u8, dv, alloc, free, text, cstring, cstringNew, copy, path, P0, P1, S, SCRATCH, PATH_MAX,
    decoder, encoder, readFile, readText, writeAll, exit, atExit, SystemError,
    argv, envp, home,
    // What the browser has that Node's library replaces on the global object. (The functions bound to it: a
    // browser's refuse to be called on anything else, "Illegal invocation".)
    web: {
      URL: globalThis.URL, URLSearchParams: globalThis.URLSearchParams, Blob: globalThis.Blob,
      MessageChannel: globalThis.MessageChannel, performance: globalThis.performance,
      crypto: globalThis.crypto, structuredClone: globalThis.structuredClone.bind(globalThis),
      setTimeout: globalThis.setTimeout.bind(globalThis), clearTimeout: globalThis.clearTimeout.bind(globalThis),
      queueMicrotask: globalThis.queueMicrotask.bind(globalThis), TextDecoder: globalThis.TextDecoder,
      TextEncoder: globalThis.TextEncoder, console: globalThis.console,
      addEventListener: typeof globalThis.addEventListener === "function" ?
        globalThis.addEventListener.bind(globalThis) : null,
    },
    debug: envp.some((e) => e === "HOSTNODE_DEBUG=1"),
    // Set by tools/node-machine.js: ES modules cannot be blobs there.
    dataModules: globalThis.__hostnodeModuleURLs === "data",
  };
  delete globalThis.__hostnodeModuleURLs;

  // An error nobody handles is the end of the program.
  hn.fatal = (error) => {
    let message;
    try {
      message = error && error.stack ? String(error.stack) : String(error);
    } catch (e) {
      message = "(an error that cannot be shown)";
    }
    writeAll(2, "hostnode: " + message + "\n");
    exit(70);
  };

  // Another part of this program: a file here, whose text is an expression for a function of hn.
  hn.load = (name) => {
    const source = readText(home + "/" + name);
    if (source === null) {
      throw new Error("hostnode: cannot read " + home + "/" + name);
    }
    return (0, eval)(source + "\n//# sourceURL=hostnode:" + name)(hn);
  };

  try {
    hn.load("loop.js");
    hn.load("node.js");
  } catch (error) {
    if (error && error.name === "Trap") {
      throw error;
    }
    hn.fatal(error);
  }
})
