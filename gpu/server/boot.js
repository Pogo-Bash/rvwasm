// gpuserver: guest programs' OpenGL, drawn by the page.
//
// A process of the machine written in JavaScript, run the way hostnode runs
// Node.js programs and wlserver runs (hostnode/boot.js says how:
// /bin/hostnode reads $HOSTNODE_HOME/boot.js and hands it to the host with
// __run_js; from then on this text is the program of the task, making the
// kernel's system calls itself). It is the server end of Mesa's vtest
// protocol: a guest program run with GALLIUM_DRIVER=virpipe and
// VTEST_SOCKET_NAME pointing here sends its GL as virgl command buffers over
// a Unix socket. The server does not decode them: it puts them in a ring in
// its memory, which the page's executor (gpu-host's side) reads straight out
// of the machine's memory and runs on WebGL2. gpu/PROTOCOL.md has the whole
// design and the ring's layout.
//
// The parts, in the directory this file was read from:
//   boot.js     this: memory, system calls, loading the other part
//   server.js   the socket, the clients, the vtest commands, the ring, the turns
//
// Started by `desktop wayland` (tools/x/desktop) as
// HOSTNODE_HOME=/run/rvgpu hostnode, with XDG_RUNTIME_DIR set.
// RVGPU_DEBUG=1 logs every command; HOSTNODE_STRACE=1 every system call.
(function (host) {
  "use strict";

  const memory = host.memory;
  const C = host.instance.exports;
  const K = host.syscalls;
  const SP = host.stack_pointer.value;
  const TP = C.__get_tls_base();

  const B = BigInt, N = Number;
  let sys0 = (n) => N(K[0](SP, TP, B(n)));
  let sys1 = (n, a) => N(K[1](SP, TP, B(n), B(a)));
  let sys2 = (n, a, b) => N(K[2](SP, TP, B(n), B(a), B(b)));
  let sys3 = (n, a, b, c) => N(K[3](SP, TP, B(n), B(a), B(b), B(c)));
  let sys4 = (n, a, b, c, d) => N(K[4](SP, TP, B(n), B(a), B(b), B(c), B(d)));
  let sys5 = (n, a, b, c, d, e) => N(K[5](SP, TP, B(n), B(a), B(b), B(c), B(d), B(e)));
  let sys6 = (n, a, b, c, d, e, f) => N(K[6](SP, TP, B(n), B(a), B(b), B(c), B(d), B(e), B(f)));

  // The kernel's numbers (include/uapi/asm-generic/unistd.h).
  const SYS = {
    epoll_create1: 20, epoll_ctl: 21, epoll_pwait: 22, fcntl: 25, flock: 32, ioctl: 29, unlinkat: 35,
    ftruncate: 46, openat: 56, close: 57, read: 63, write: 64, ppoll: 73, fstat: 80, exit: 93, exit_group: 94,
    getpid: 172, socket: 198, bind: 200, listen: 201, sendmsg: 211, recvmsg: 212, munmap: 215, mmap: 222,
    accept4: 242, eventfd2: 19,
  };

  let buffer = memory.buffer;
  let bytes = new Uint8Array(buffer);
  let view = new DataView(buffer);
  let words = new Int32Array(buffer);
  const renew = () => {
    buffer = memory.buffer;
    bytes = new Uint8Array(buffer);
    view = new DataView(buffer);
    words = new Int32Array(buffer);
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
  // The index of the 32-bit word at `address` in i32() (Atomics). The machine's memory can be larger than
  // 4 GiB (mem=8192M: the page's default on a big computer), so an address is a Number of up to 2^34 and more:
  // never turn one into an index, or a page or a rounded value, with `>> 2`, `& ~4095`, `| 0` or `>>> 0` (32-bit
  // operators: above 2 GiB the index goes negative, above 4 GiB it wraps to a LOW address, the kernel's).
  const ix32 = (address) => Math.floor(address / 4);
  const roundUp = (n, to) => Math.ceil(n / to) * to;
  // For Atomics on a 32-bit word of the memory (index = address / 4).
  const i32 = () => {
    if (memory.buffer !== buffer) {
      renew();
    }
    return words;
  };

  // (Only what hostnode.c's program links is there to call: malloc and free, not aligned_alloc. Who wants
  // an alignment asks for more and rounds up.)
  const alloc = (size) => {
    const address = N(C.malloc(B(size)));
    if (!address) {
      throw new RangeError("gpuserver: out of memory (" + size + " bytes)");
    }
    return address;
  };
  const free = (address) => C.free(B(address));

  const decoder = new TextDecoder("utf-8");
  const encoder = new TextEncoder();
  const text = (address, length) => decoder.decode(u8().slice(address, address + length));
  const cstring = (address) => {
    const m = u8();
    let end = address;
    while (m[end]) {
      end++;
    }
    return text(address, end - address);
  };

  const PATH_MAX = 4096;
  const SCRATCH = 65536;
  const P0 = alloc(PATH_MAX + SCRATCH);
  const S = P0 + PATH_MAX;

  const path = (p, where) => {
    const raw = encoder.encode(p);
    if (raw.length >= PATH_MAX) {
      return -36;
    }
    const m = u8();
    m.set(raw, where);
    m[where + raw.length] = 0;
    return where;
  };

  const O_RDONLY = 0, O_CLOEXEC = 0o2000000, AT_FDCWD = -100, EINTR = 4, EAGAIN = 11;

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
      chunks.push(u8().slice(S, S + n));
      total += n;
    }
    sys1(SYS.close, fd);
    const all = new Uint8Array(total);
    let at = 0;
    for (const chunk of chunks) {
      all.set(chunk, at);
      at += chunk.length;
    }
    return all;
  };

  // Write all of `length` bytes at `address` to a descriptor that may be non-blocking.
  const writeMem = (fd, address, length) => {
    let done = 0;
    while (done < length) {
      const n = sys3(SYS.write, fd, address + done, length - done);
      if (n === -EINTR) {
        continue;
      }
      if (n === -EAGAIN) {
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
  const writeAll = (fd, data) => {
    if (typeof data === "string") {
      data = encoder.encode(data);
    }
    for (let done = 0; done < data.length;) {
      const part = Math.min(data.length - done, SCRATCH);
      u8().set(data.subarray(done, done + part), S);
      const n = writeMem(fd, S, part);
      if (n < 0) {
        return n;
      }
      done += part;
    }
    return data.length;
  };

  const exit = (code) => {
    sys1(SYS.exit_group, code & 0xff);
    for (;;) {
      sys1(SYS.exit, code & 0xff);
    }
  };

  // What the Wasm program told us (struct hn_start in hostnode.c).
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
  const env = (name) => {
    const prefix = name + "=";
    const e = envp.find((e) => e.startsWith(prefix));
    return e ? e.slice(prefix.length) : null;
  };

  const log = (line) => {
    writeAll(2, line + "\n");
  };

  if (env("HOSTNODE_STRACE") === "1") {
    const names = {};
    for (const name of Object.keys(SYS)) {
      names[SYS[name]] = name;
    }
    let depth = 0;
    const wrap = (count) => (n, ...args) => {
      if (depth > 0 || (n === SYS.write && args[0] === 2)) {
        return N(K[count](SP, TP, B(n), ...args.map(B)));
      }
      depth++;
      let r;
      try {
        r = N(K[count](SP, TP, B(n), ...args.map(B)));
      } finally {
        depth--;
      }
      if (n !== SYS.epoll_pwait || r !== 0) {
        log("[strace] " + (names[n] || n) + "(" + args.join(", ") + ") = " + r);
      }
      return r;
    };
    sys0 = wrap(0);
    sys1 = wrap(1);
    sys2 = wrap(2);
    sys3 = wrap(3);
    sys4 = wrap(4);
    sys5 = wrap(5);
    sys6 = wrap(6);
  }

  // A client that went away while we write to it is not the end of us.
  C.hn_signal(13, 1);

  const gs = {
    host, memory, C, SYS, sys0, sys1, sys2, sys3, sys4, sys5, sys6,
    u8, dv, i32, ix32, roundUp, alloc, free, text, cstring, path, P0, S, SCRATCH, PATH_MAX,
    readFile, writeMem, writeAll, exit, argv, envp, env, home, log,
    debug: env("RVGPU_DEBUG") === "1",
  };

  // The page load this machine belongs to: rvtab=ID on the kernel's command line (tools/www/dev.html), which
  // names the BroadcastChannel to the page, "rv-gpu-ID" (tools/www/display.js: window.rvChannel). With no id (a
  // machine run without a page: tools/node-machine.js) the name is as it was, "rv-gpu".
  {
    const found = /(?:^|\s)rvtab=([0-9A-Za-z-]+)/.exec(decoder.decode(readFile("/proc/cmdline") || new Uint8Array(0)));
    gs.tab = found ? found[1] : "";
    gs.channel = (name) => new BroadcastChannel(gs.tab ? name + "-" + gs.tab : name);
  }

  gs.fatal = (error) => {
    let message;
    try {
      message = error && error.stack ? String(error.stack) : String(error);
    } catch (e) {
      message = "(an error that cannot be shown)";
    }
    log("gpuserver: " + message);
    exit(70);
  };

  gs.load = (name) => {
    const data = readFile(home + "/" + name);
    if (data === null) {
      throw new Error("gpuserver: cannot read " + home + "/" + name);
    }
    return (0, eval)(decoder.decode(data) + "\n//# sourceURL=gpuserver:" + name)(gs);
  };

  try {
    gs.load(env("RVGPU_LOAD") || "server.js");	// (RVGPU_LOAD: a test program in the server's place, ballast.js)
  } catch (error) {
    if (error && error.name === "Trap") {
      throw error;
    }
    gs.fatal(error);
  }
})
