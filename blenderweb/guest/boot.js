// blender-gpu: Blender, run natively by the browser the machine is in (Puter's blender-wasm, on the host's
// GPU through WebGPU), as a window of the machine's desktop that opens and saves the machine's files.
//
//   blender-gpu [FILE.blend]       (blenderweb/guest/blender-gpu; FILE must be in the home directory)
//
// This is JavaScript that hostnode runs as a process of the machine (hostnode/hostnode.c, as the window
// server and hostopen are run), so it can speak both to the window server, as a Wayland client, and to the
// page, on the BroadcastChannel "rv-blenderweb". It does three things:
//
// 1. Asks the page for Blender: { type: "open", id, mark, markBytes, open, home, pid }. Only the page whose
//    machine has the random mark in its memory takes it (hostopen/boot.js says why: every tab of the page's
//    origin hears the channel). The page answers { type: "open-answer", id, status, reason }.
// 2. Makes the window: an xdg_toplevel whose app id is "rvembed-ID.rv-blender-gpu" (the page knows the window
//    by it, and the launcher's icon by its last part), drawn as one grey pixel stretched to the window's size
//    (wp_viewporter). The page lays Blender's iframe over it, so the window moves, resizes, stacks, minimizes
//    and switches like any other; its title follows Blender's. Closing it asks Blender's page first (what is not
//    saved would be lost); the page says { type: "quit" } when it may go.
// 3. Serves the home directory to Blender: { type: "fs", id, rid, op, path, to, data } with op one of stat,
//    list, read, write, mkdir, unlink, rename; paths relative to the home directory, which they cannot leave
//    (openat2 with RESOLVE_BENEATH: no "..", no symbolic link out of it). Answered with
//    { type: "fs-answer", id, rid, ok, value, error, errno }.
//
// Exits 0 when the window is closed, 1 if the page refused or Blender failed, 2 for a bad command line, 3 when
// no page answered (no blenderweb.js there, or no window server).
(function (host) {
  "use strict";

  const memory = host.memory;
  const C = host.instance.exports;
  const K = host.syscalls;
  const SP = host.stack_pointer.value;
  const TP = C.__get_tls_base();
  const B = BigInt, N = Number;
  const sys0 = (n) => N(K[0](SP, TP, B(n)));
  const sys1 = (n, a) => N(K[1](SP, TP, B(n), B(a)));
  const sys2 = (n, a, b) => N(K[2](SP, TP, B(n), B(a), B(b)));
  const sys3 = (n, a, b, c) => N(K[3](SP, TP, B(n), B(a), B(b), B(c)));
  const sys4 = (n, a, b, c, d) => N(K[4](SP, TP, B(n), B(a), B(b), B(c), B(d)));
  const sys5 = (n, a, b, c, d, e) => N(K[5](SP, TP, B(n), B(a), B(b), B(c), B(d), B(e)));

  // The kernel's numbers (include/uapi/asm-generic/unistd.h).
  const SYS = {
    getcwd: 17, mkdirat: 34, unlinkat: 35, ftruncate: 46, openat: 56, close: 57, getdents64: 61, read: 63,
    write: 64, pwrite64: 68, fstat: 80, exit: 93, exit_group: 94, getpid: 172, socket: 198, connect: 203,
    sendmsg: 211, recvmsg: 212, renameat2: 276, openat2: 437,
  };
  const AT_FDCWD = -100, AT_REMOVEDIR = 0x200;
  const O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 0o100, O_EXCL = 0o200, O_TRUNC = 0o1000,
        O_DIRECTORY = 0o200000, O_CLOEXEC = 0o2000000, O_PATH = 0o10000000;
  const RESOLVE_NO_MAGICLINKS = 0x02, RESOLVE_BENEATH = 0x08;
  const S_IFMT = 0o170000, S_IFDIR = 0o040000, S_IFREG = 0o100000;
  const EINTR = 4, EAGAIN = 11, ENOENT = 2, EINVAL = 22, ENOSYS = 38, EISDIR = 21;
  const MSG_DONTWAIT = 0x40, MSG_NOSIGNAL = 0x4000, MSG_CMSG_CLOEXEC = 0x40000000;
  const SOL_SOCKET = 1, SCM_RIGHTS = 1;

  let bytes = new Uint8Array(memory.buffer), view = new DataView(memory.buffer);
  const u8 = () => (bytes.buffer === memory.buffer ? bytes : (bytes = new Uint8Array(memory.buffer)));
  const dv = () => (view.buffer === memory.buffer ? view : (view = new DataView(memory.buffer)));
  const encoder = new TextEncoder(), decoder = new TextDecoder("utf-8");
  const alloc = (size) => {
    const a = N(C.malloc(B(size)));
    if (!a) {
      throw new RangeError("blender-gpu: out of memory (" + size + " bytes)");
    }
    return a;
  };
  const cstring = (address) => {
    const m = u8();
    let end = address;
    while (m[end]) {
      end++;
    }
    return decoder.decode(m.slice(address, end));
  };
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
  const start = N(host.arg);
  const argv = pointers(N(dv().getBigUint64(start + 8, true)));
  const envp = pointers(N(dv().getBigUint64(start + 16, true)));
  const env = (name) => {
    const e = envp.find((x) => x.startsWith(name + "="));
    return e === undefined ? null : e.slice(name.length + 1);
  };

  const PATH_MAX = 4096, SCRATCH = 65536, BIG = 1 << 20;
  const P0 = alloc(2 * PATH_MAX), P1 = P0 + PATH_MAX;
  const S = alloc(SCRATCH);
  const IO = alloc(BIG);		// file contents in and out
  const HOW = alloc(32);		// struct open_how
  const cpath = (p, where) => {
    const raw = encoder.encode(p);
    if (raw.length >= PATH_MAX || raw.includes(0)) {
      return -1;
    }
    u8().set(raw, where);
    u8()[where + raw.length] = 0;
    return where;
  };
  const writeAll = (fd, data) => {
    if (typeof data === "string") {
      data = encoder.encode(data);
    }
    for (let done = 0; done < data.length;) {
      const part = Math.min(data.length - done, BIG);
      u8().set(data.subarray(done, done + part), IO);
      const n = sys3(SYS.write, fd, IO, part);
      if (n === -EINTR) {
        continue;
      }
      if (n <= 0) {
        return n < 0 ? n : -5;
      }
      done += n;
    }
    return 0;
  };
  const say = (line) => {
    writeAll(2, "blender-gpu: " + line + "\n");
  };
  const debug = env("BLENDERWEB_DEBUG") === "1";
  const exit = (code) => {
    sys1(SYS.exit_group, code);
    for (;;) {
      sys1(SYS.exit, code);
    }
  };

  // ---- the command line: at most a file to open, which has to be in the home directory
  const home = (env("HOME") || "/root").replace(/\/+$/, "") || "/";
  if (argv.length > 2 || (argv[1] && argv[1].startsWith("-"))) {
    say("usage: blender-gpu [FILE.blend]");
    exit(2);
  }
  let openRel = null;
  if (argv[1]) {
    let p = argv[1];
    if (!p.startsWith("/")) {
      const n = sys2(SYS.getcwd, S, SCRATCH);
      p = (n > 0 ? cstring(S) : home) + "/" + p;
    }
    // (Made plain: no "." or ".." left; whether it stays in the home directory is checked again on every use.)
    const parts = [];
    for (const c of p.split("/")) {
      if (c === "..") {
        parts.pop();
      } else if (c && c !== ".") {
        parts.push(c);
      }
    }
    p = "/" + parts.join("/");
    if (!(p + "/").startsWith(home + "/") || p === home) {
      say("only a file in the home directory (" + home + ") can be opened: " + argv[1]);
      exit(2);
    }
    openRel = p.slice(home.length + 1);
  }

  // ---- the home directory, and paths in it
  const homeFd = sys4(SYS.openat, AT_FDCWD, cpath(home, P0), O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
  if (homeFd < 0) {
    say("cannot open the home directory " + home + " (error " + -homeFd + ")");
    exit(1);
  }
  let haveOpenat2 = true;
  const fsError = (errno, what) => {
    const e = new Error(what + ": error " + errno);
    e.errno = errno;
    return e;
  };
  // A path relative to the home directory, checked: no NUL, nothing absolute, no "..".
  const rel = (p) => {
    if (typeof p !== "string" || p.length > 2048 || p.includes("\0")) {
      throw fsError(EINVAL, "bad path");
    }
    const parts = p.split("/").filter((c) => c && c !== ".");
    if (parts.includes("..")) {
      throw fsError(EINVAL, "no .. here");
    }
    return parts.join("/");
  };
  // Open a path under the home directory and nowhere else (openat2 RESOLVE_BENEATH: a symbolic link that leads
  // out of it is refused). Throws on error.
  const openIn = (p, flags, mode = 0) => {
    const name = p === "" ? "." : p;
    if (haveOpenat2) {
      const v = dv();
      v.setBigUint64(HOW, B(flags | O_CLOEXEC), true);
      v.setBigUint64(HOW + 8, B(flags & O_CREAT ? mode : 0), true);
      v.setBigUint64(HOW + 16, B(RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS), true);
      const fd = sys4(SYS.openat2, homeFd, cpath(name, P0), HOW, 24);
      if (fd !== -ENOSYS) {
        if (fd < 0) {
          throw fsError(-fd, "open " + p);
        }
        return fd;
      }
      haveOpenat2 = false;
      say("no openat2 in this kernel: symbolic links out of the home directory are not stopped");
    }
    const fd = sys4(SYS.openat, homeFd, cpath(name, P0), flags | O_CLOEXEC, mode);
    if (fd < 0) {
      throw fsError(-fd, "open " + p);
    }
    return fd;
  };
  const fstat = (fd) => {
    const r = sys2(SYS.fstat, fd, S);
    if (r < 0) {
      throw fsError(-r, "fstat");
    }
    const v = dv();
    const mode = v.getUint32(S + 16, true);
    return { isDir: (mode & S_IFMT) === S_IFDIR, isFile: (mode & S_IFMT) === S_IFREG, size: N(v.getBigInt64(S + 48, true)) };
  };
  const statIn = (p) => {
    const fd = openIn(p, O_PATH);
    try {
      return fstat(fd);
    } finally {
      sys1(SYS.close, fd);
    }
  };
  // The directory a path is in (opened), and its last name.
  const parentIn = (p) => {
    const at = p.lastIndexOf("/");
    const dir = at < 0 ? "" : p.slice(0, at), name = at < 0 ? p : p.slice(at + 1);
    if (!name) {
      throw fsError(EINVAL, "no name");
    }
    return [openIn(dir, O_PATH | O_DIRECTORY), name];
  };

  const ops = {
    stat(m) {
      const p = rel(m.path);
      try {
        const st = statIn(p);
        return { size: st.size, isDir: st.isDir };
      } catch (e) {
        if (e.errno === ENOENT) {
          return null;
        }
        throw e;
      }
    },
    list(m) {
      const p = rel(m.path);
      const fd = openIn(p, O_RDONLY | O_DIRECTORY);
      const names = [];
      try {
        for (;;) {
          const n = sys3(SYS.getdents64, fd, S, SCRATCH);
          if (n < 0) {
            throw fsError(-n, "getdents64");
          }
          if (n === 0) {
            break;
          }
          const v = dv();
          for (let at = S; at < S + n;) {
            const reclen = v.getUint16(at + 16, true);
            const name = cstring(at + 19);
            if (name !== "." && name !== "..") {
              names.push(name);
            }
            at += reclen;
          }
        }
      } finally {
        sys1(SYS.close, fd);
      }
      const out = [];
      for (const name of names) {
        try {
          const st = statIn(p ? p + "/" + name : name);
          if (st.isDir || st.isFile) {
            out.push({ name, isDir: st.isDir, size: st.size });
          }
        } catch (e) {
          // (a dangling link, a link out of the home directory, gone meanwhile: not listed)
        }
      }
      return out;
    },
    read(m) {
      const fd = openIn(rel(m.path), O_RDONLY);
      try {
        const st = fstat(fd);
        if (st.isDir) {
          throw fsError(EISDIR, "read");
        }
        const chunks = [];
        let total = 0;
        for (;;) {
          const n = sys3(SYS.read, fd, IO, BIG);
          if (n === -EINTR) {
            continue;
          }
          if (n < 0) {
            throw fsError(-n, "read");
          }
          if (n === 0) {
            break;
          }
          chunks.push(u8().slice(IO, IO + n));
          total += n;
        }
        const all = new Uint8Array(total);
        let at = 0;
        for (const c of chunks) {
          all.set(c, at);
          at += c.length;
        }
        return all;
      } finally {
        sys1(SYS.close, fd);
      }
    },
    write(m) {
      const data = m.data instanceof Uint8Array ? m.data : new Uint8Array(m.data || []);
      const fd = openIn(rel(m.path), O_WRONLY | O_CREAT | O_TRUNC, 0o644);
      try {
        const r = writeAll(fd, data);
        if (r < 0) {
          throw fsError(-r, "write");
        }
      } finally {
        sys1(SYS.close, fd);
      }
      return data.length;
    },
    mkdir(m) {
      const [dfd, name] = parentIn(rel(m.path));
      try {
        const r = sys3(SYS.mkdirat, dfd, cpath(name, P0), 0o755);
        if (r < 0) {
          throw fsError(-r, "mkdir");
        }
      } finally {
        sys1(SYS.close, dfd);
      }
      return true;
    },
    unlink(m) {
      const p = rel(m.path);
      const st = statIn(p);
      const [dfd, name] = parentIn(p);
      try {
        const r = sys3(SYS.unlinkat, dfd, cpath(name, P0), st.isDir ? AT_REMOVEDIR : 0);
        if (r < 0) {
          throw fsError(-r, "unlink");
        }
      } finally {
        sys1(SYS.close, dfd);
      }
      return true;
    },
    rename(m) {
      const [afd, a] = parentIn(rel(m.path));
      let bfd = -1;
      try {
        const [fd, b] = parentIn(rel(m.to));
        bfd = fd;
        const r = sys5(SYS.renameat2, afd, cpath(a, P0), bfd, cpath(b, P1), 0);
        if (r < 0) {
          throw fsError(-r, "rename");
        }
      } finally {
        sys1(SYS.close, afd);
        if (bfd >= 0) {
          sys1(SYS.close, bfd);
        }
      }
      return true;
    },
  };

  // ---- the page
  const MARK = 32;
  const mark = alloc(MARK);
  const random = new Uint8Array(MARK);
  crypto.getRandomValues(random);
  random.set([0x52, 0x56, 0x42, 0x57]);	// "RVBW"
  u8().set(random, mark);
  const id = Array.from(random.subarray(4, 16), (b) => b.toString(16).padStart(2, "0")).join("");
  const appId = "rvembed-" + id + ".rv-blender-gpu";
  const readText = (name) => {
    const fd = sys4(SYS.openat, AT_FDCWD, cpath(name, P0), O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
      return null;
    }
    const chunks = [];
    for (;;) {
      const n = sys3(SYS.read, fd, IO, BIG);
      if (n === -EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      chunks.push(u8().slice(IO, IO + n));
    }
    sys1(SYS.close, fd);
    return chunks.map((c) => decoder.decode(c, { stream: true })).join("") + decoder.decode();
  };
  // The page load this machine belongs to: rvtab=ID on the kernel's command line (tools/www/dev.html), which
  // names the channel, "rv-blenderweb-ID" (tools/www/display.js: window.rvChannel). With no id the name is plain.
  const tabFound = /(?:^|\s)rvtab=([0-9A-Za-z-]+)/.exec(readText("/proc/cmdline") || "");
  const tab = tabFound ? tabFound[1] : "";
  let channel;
  try {
    channel = new BroadcastChannel(tab ? "rv-blenderweb-" + tab : "rv-blenderweb");
  } catch (error) {
    say("no BroadcastChannel here (" + error.message + ")");
    exit(3);
  }
  const post = (m) => channel.postMessage({ ...m, id });
  let answered = false;
  const quit = (code, line) => {
    if (line) {
      say(line);
    }
    u8().fill(0, mark, mark + MARK);
    try {
      post({ type: "gone" });
      channel.close();
    } catch (e) {
      // (going anyway)
    }
    exit(code);
  };

  // ---- the window (a Wayland client: the requests and events by name, numbered from the window server's own
  // tables, /run/wlserver/protocol.js)
  const protoText = readText("/run/wlserver/protocol.js");
  if (protoText === null) {
    quit(3, "no window server here (/run/wlserver/protocol.js): start it with `desktop wayland`");
  }
  const protocol = (0, eval)(protoText + "\n//# sourceURL=wlserver:protocol.js")();
  const runtime = env("XDG_RUNTIME_DIR") || "/run/user/0";
  const display = env("WAYLAND_DISPLAY") || "wayland-0";
  const socketPath = display.startsWith("/") ? display : runtime + "/" + display;

  const fd = sys3(SYS.socket, 1, 1 | O_CLOEXEC, 0);	// AF_UNIX, SOCK_STREAM
  {
    const v = dv();
    u8().fill(0, S, S + 110);
    v.setUint16(S, 1, true);
    const raw = encoder.encode(socketPath);
    if (raw.length > 107) {
      quit(3, "the window server's socket path is too long: " + socketPath);
    }
    u8().set(raw, S + 2);
    const r = sys3(SYS.connect, fd, S, 110);
    if (fd < 0 || r < 0) {
      quit(3, "no window server at " + socketPath + " (error " + -(fd < 0 ? fd : r) + "): start it with `desktop wayland`");
    }
  }

  const objects = new Map();	// id -> { iface, on: { event: handler } }
  let nextObject = 2;
  const out = [];		// [bytes, fds]
  const newObject = (iface, on = {}) => {
    const oid = nextObject++;
    objects.set(oid, { iface, on });
    return oid;
  };
  objects.set(1, { iface: "wl_display", on: {} });
  const request = (oid, name, ...values) => {
    const o = objects.get(oid);
    const iface = protocol[o.iface];
    const opcode = iface.requests.findIndex((r) => r.name === name);
    if (opcode < 0) {
      throw new Error("no request " + o.iface + "." + name);
    }
    const sig = iface.requests[opcode].args;
    const parts = [];
    const fds = [];
    let size = 8;
    const word = (x) => {
      const b = new Uint8Array(4);
      new DataView(b.buffer).setUint32(0, x >>> 0, true);
      parts.push(b);
      size += 4;
    };
    const str = (s) => {
      const raw = encoder.encode(s);
      word(raw.length + 1);
      const padded = new Uint8Array((raw.length + 1 + 3) & ~3);
      padded.set(raw);
      parts.push(padded);
      size += padded.length;
    };
    sig.forEach(([, nullable, ifaceOf], i) => {
      const x = values[i], type = nullable.replace("?", "");	// ("?o", "?s": may be null; not sent null here)
      if (type === "i" || type === "u" || type === "o") {
        word(x | 0);
      } else if (type === "f") {
        word(Math.round(x * 256));
      } else if (type === "s") {
        str(x);
      } else if (type === "n") {
        if (!ifaceOf) {
          // (wl_registry.bind: the interface and version go before the new id)
          str(x.iface);
          word(x.version);
          word(x.id);
        } else {
          word(x);
        }
      } else if (type === "h") {
        fds.push(x);
      } else {
        throw new Error("argument type " + type + " not done here");
      }
    });
    const msg = new Uint8Array(size);
    const v = new DataView(msg.buffer);
    v.setUint32(0, oid, true);
    v.setUint32(4, (size << 16) | opcode, true);
    let at = 8;
    for (const p of parts) {
      msg.set(p, at);
      at += p.length;
    }
    out.push([msg, fds]);
    if (debug) {
      say("-> " + o.iface + "@" + oid + "." + name + "(" + values.map((x) => JSON.stringify(x)).join(", ") + ")");
    }
  };
  const MSG = S, IOV = S + 64, CTL = S + 96, DATA = S + 256, DATA_SIZE = SCRATCH - 256;
  const flush = () => {
    while (out.length) {
      const [msg, fds] = out[0];
      const m = u8(), v = dv();
      m.set(msg, DATA);
      m.fill(0, MSG, MSG + 56);
      v.setBigUint64(IOV, B(DATA), true);
      v.setBigUint64(IOV + 8, B(msg.length), true);
      v.setBigUint64(MSG + 16, B(IOV), true);
      v.setBigUint64(MSG + 24, 1n, true);
      if (fds.length) {
        v.setBigUint64(CTL, B(16 + 4 * fds.length), true);
        v.setInt32(CTL + 8, SOL_SOCKET, true);
        v.setInt32(CTL + 12, SCM_RIGHTS, true);
        fds.forEach((f, i) => v.setInt32(CTL + 16 + 4 * i, f, true));
        v.setBigUint64(MSG + 32, B(CTL), true);
        v.setBigUint64(MSG + 40, B((16 + 4 * fds.length + 7) & ~7), true);
      }
      const n = sys3(SYS.sendmsg, fd, MSG, MSG_NOSIGNAL);	// (small messages, a local socket: it blocks a moment at most)
      if (n === -EINTR) {
        continue;
      }
      if (n !== msg.length) {
        quit(1, "the window server went away (send: " + n + ")");
      }
      out.shift();
    }
  };
  let inBytes = new Uint8Array(0);
  const receive = () => {
    for (;;) {
      const m = u8(), v = dv();
      m.fill(0, MSG, MSG + 56);
      v.setBigUint64(IOV, B(DATA), true);
      v.setBigUint64(IOV + 8, B(DATA_SIZE), true);
      v.setBigUint64(MSG + 16, B(IOV), true);
      v.setBigUint64(MSG + 24, 1n, true);
      v.setBigUint64(MSG + 32, B(CTL), true);
      v.setBigUint64(MSG + 40, B(160), true);
      const n = sys3(SYS.recvmsg, fd, MSG, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
      if (n === -EINTR) {
        continue;
      }
      if (n === -EAGAIN) {
        return;
      }
      if (n <= 0) {
        quit(0, n === 0 ? null : "the window server's socket: error " + -n);
      }
      // (No event of the interfaces used here carries a descriptor except wl_keyboard.keymap, not asked for:
      // any that come are closed.)
      const controllen = N(dv().getBigUint64(MSG + 40, true));
      for (let at = CTL; at + 16 <= CTL + controllen;) {
        const len = N(dv().getBigUint64(at, true));
        if (len < 16) {
          break;
        }
        for (let i = at + 16; i + 4 <= at + len; i += 4) {
          sys1(SYS.close, dv().getInt32(i, true));
        }
        at += (len + 7) & ~7;
      }
      const joined = new Uint8Array(inBytes.length + n);
      joined.set(inBytes);
      joined.set(u8().subarray(DATA, DATA + n), inBytes.length);
      inBytes = joined;
      decode();
    }
  };
  const decode = () => {
    const v = new DataView(inBytes.buffer, inBytes.byteOffset, inBytes.byteLength);
    let at = 0;
    while (inBytes.length - at >= 8) {
      const oid = v.getUint32(at, true), w = v.getUint32(at + 4, true);
      const size = w >>> 16, opcode = w & 0xffff;
      if (size < 8 || inBytes.length - at < size) {
        break;
      }
      const o = objects.get(oid);
      const event = o && protocol[o.iface] && protocol[o.iface].events[opcode];
      if (event) {
        const args = [];
        let p = at + 8;
        for (const [, nullable] of event.args) {
          const type = nullable.replace("?", "");
          if (type === "i") {
            args.push(v.getInt32(p, true));
            p += 4;
          } else if (type === "u" || type === "o" || type === "n") {
            args.push(v.getUint32(p, true));
            p += 4;
          } else if (type === "f") {
            args.push(v.getInt32(p, true) / 256);
            p += 4;
          } else if (type === "s" || type === "a") {
            const len = v.getUint32(p, true);
            const data = inBytes.slice(p + 4, p + 4 + len);
            args.push(type === "s" ? decoder.decode(data.subarray(0, Math.max(0, len - 1))) : data);
            p += 4 + ((len + 3) & ~3);
          } else if (type === "h") {
            args.push(-1);
          }
        }
        if (debug) {
          say("<- " + o.iface + "@" + oid + "." + event.name + "(" + args.map((x) => (x instanceof Uint8Array ? "[" + x.length + "]" : JSON.stringify(x))).join(", ") + ")");
        }
        const handler = o.on[event.name];
        if (handler) {
          handler(...args);
        }
      }
      at += size;
    }
    inBytes = inBytes.slice(at);
  };

  // wl_display: errors end us; deleted ids are forgotten.
  objects.get(1).on = {
    error: (object, code, message) => quit(1, "the window server says: " + message + " (object " + object + ", code " + code + ")"),
    delete_id: (oid) => objects.delete(oid),
  };
  const globals = {};
  const registry = newObject("wl_registry", {
    global: (name, iface, version) => {
      globals[iface] = { name, version };
    },
  });
  request(1, "get_registry", registry);
  const roundtrip = (then) => {
    const cb = newObject("wl_callback", { done: () => then() });
    request(1, "sync", cb);
    flush();
  };
  const bind = (iface, version, on = {}) => {
    const g = globals[iface];
    if (!g) {
      return 0;
    }
    const oid = newObject(iface, on);
    request(registry, "bind", g.name, { iface, version: Math.min(version, g.version), id: oid });
    return oid;
  };

  // The window's size at first: most of the screen (the window server writes the screen's size for the desktop
  // script: width height scale panel title border).
  const screen = (readText(runtime + "/wlserver.screen") || "").trim().split(/\s+/).map(Number);
  const firstSize = () => {
    const sw = screen[0] > 0 ? screen[0] : 1280, sh = screen[1] > 0 ? screen[1] : 800;
    const panel = screen[3] > 0 ? screen[3] : 0, chrome = (screen[4] > 0 ? screen[4] : 28) + 2 * (screen[5] > 0 ? screen[5] : 4);
    return [Math.max(480, Math.min(1600, Math.round(sw * 0.86))), Math.max(360, Math.min(1000, Math.round((sh - panel - chrome) * 0.88)))];
  };

  let surface = 0, xdgSurface = 0, toplevel = 0, viewport = 0, buffer = 0;
  let size = firstSize(), pendingSize = null, configured = false;
  let title = "Blender";
  let globalsReady = false;

  const makeBuffer = (shm) => {
    // One pixel, Blender's grey, in a file of the runtime directory (in memory): truncated up from nothing, so
    // the kernel gives it a whole page that the server can map (wlserver/README.md).
    const name = runtime + "/blender-gpu-" + id;
    const f = sys4(SYS.openat, AT_FDCWD, cpath(name, P0), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0o600);
    if (f < 0) {
      quit(1, "cannot make the window's picture in " + runtime + " (error " + -f + ")");
    }
    sys3(SYS.unlinkat, AT_FDCWD, cpath(name, P0), 0);
    sys2(SYS.ftruncate, f, 4);
    u8().set([0x30, 0x30, 0x30, 0xff], IO);	// (XRGB8888: B, G, R, X)
    sys4(SYS.pwrite64, f, IO, 4, 0);
    const pool = newObject("wl_shm_pool");
    request(shm, "create_pool", pool, f, 4);
    buffer = newObject("wl_buffer", { release: () => {} });
    request(pool, "create_buffer", buffer, 0, 1, 1, 4, 1);	// (format 1: XRGB8888)
    request(pool, "destroy");
    flush();
    sys1(SYS.close, f);
  };

  const show = () => {
    request(viewport, "set_destination", size[0], size[1]);
    request(surface, "attach", buffer, 0, 0);
    request(surface, "damage_buffer", 0, 0, 1, 1);
    request(surface, "commit");
    flush();
  };

  const makeWindow = () => {
    const compositor = bind("wl_compositor", 4);
    const shm = bind("wl_shm", 1);
    const wmBase = bind("xdg_wm_base", 2, {
      ping: (serial) => {
        request(wmBase, "pong", serial);
        flush();
      },
    });
    const viewporter = bind("wp_viewporter", 1);
    if (!compositor || !shm || !wmBase || !viewporter) {
      quit(3, "the window server lacks what a window needs here (wl_compositor, wl_shm, xdg_wm_base, wp_viewporter)");
    }
    surface = newObject("wl_surface");
    request(compositor, "create_surface", surface);
    viewport = newObject("wp_viewport");
    request(viewporter, "get_viewport", viewport, surface);
    xdgSurface = newObject("xdg_surface", {
      configure: (serial) => {
        request(xdgSurface, "ack_configure", serial);
        if (pendingSize) {
          size = pendingSize;
          pendingSize = null;
        }
        configured = true;
        show();
      },
    });
    request(wmBase, "get_xdg_surface", xdgSurface, surface);
    toplevel = newObject("xdg_toplevel", {
      configure: (w, h) => {
        if (w > 0 && h > 0) {
          pendingSize = [w, h];
        }
      },
      close: () => {
        // Blender's page asks first; it says "quit" when the window may go.
        post({ type: "close" });
      },
    });
    request(xdgSurface, "get_toplevel", toplevel);
    request(toplevel, "set_title", title);
    request(toplevel, "set_app_id", appId);
    request(toplevel, "set_min_size", 320, 240);
    makeBuffer(shm);
    request(surface, "commit");	// (the first commit, without a picture: the server answers with a configure)
    flush();
  };

  channel.onmessage = (event) => {
    const m = event.data;
    if (!m || m.id !== id) {
      return;
    }
    if (m.type === "open-answer" && !answered) {
      answered = true;
      if (m.status !== "ok") {
        quit(1, "the page refused: " + (m.reason || m.status));
      }
      if (globalsReady) {
        makeWindow();
      }
      return;
    }
    if (!answered) {
      return;
    }
    if (m.type === "fs") {
      const op = ops[m.op];
      let answer;
      try {
        if (!op) {
          throw fsError(EINVAL, "no such operation " + m.op);
        }
        answer = { type: "fs-answer", rid: m.rid, ok: true, value: op(m) };
      } catch (e) {
        answer = { type: "fs-answer", rid: m.rid, ok: false, error: e.message, errno: e.errno || 5 };
      }
      if (debug) {
        say("fs " + m.op + " " + JSON.stringify(m.path) + (m.to ? " " + JSON.stringify(m.to) : "") + ": " + (answer.ok ? "ok" : answer.error));
      }
      post(answer);
    } else if (m.type === "title" && toplevel && typeof m.title === "string") {
      const t = m.title.replace(/[\u0000-\u001f]/g, " ").slice(0, 200) || "Blender";
      if (t !== title) {
        title = t;
        request(toplevel, "set_title", title);
        flush();
      }
    } else if (m.type === "quit") {
      quit(0, null);
    } else if (m.type === "failed") {
      quit(1, "Blender did not start: " + (m.why || "?"));
    }
  };

  post({ type: "open", mark, markBytes: Array.from(random), pid: sys0(SYS.getpid), open: openRel === null ? null : home + "/" + openRel, home });
  setTimeout(() => {
    if (!answered) {
      quit(3, "no page answered (a page without blenderweb.js?)");
    }
  }, 5000);
  // The window server's events, every 20 ms (the channel's messages come as they come).
  roundtrip(() => {
    globalsReady = true;
    if (answered && !surface) {
      makeWindow();
    }
  });
  setInterval(() => {
    receive();
    flush();
  }, 20);
  if (debug) {
    say("asked the page for Blender (" + appId + ")" + (openRel !== null ? ", file " + home + "/" + openRel : ""));
  }
})
