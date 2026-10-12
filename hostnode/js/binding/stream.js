// Streams of bytes on descriptors (src/stream_base.cc, stream_wrap.cc,
// tty_wrap.cc, pipe_wrap.cc, tcp_wrap.cc, on libuv's uv_stream_t): terminals,
// pipes, sockets. And looking up host names (cares_wrap).
//
// A descriptor of our own (a socket we made, our end of a pipe to a child) is
// non-blocking. One we were given (standard input and output) is left as it
// is, because it is someone else's too: such a descriptor is read only when
// the kernel says there is something, and a write to it waits until it is
// written, which is how Node treats its standard output on Linux anyway.
(function (hn) {
  "use strict";

  const { loop, data, SYS, sys1, sys2, sys3, sys4, sys5, dv, u8, S, SCRATCH, P0 } = hn;
  const shapes = data.bindings;
  const EPOLLIN = 1, EPOLLOUT = 4, EPOLLERR = 8, EPOLLHUP = 16, EPOLLRDHUP = 0x2000;
  const EINTR = 4, EAGAIN = 11, EINPROGRESS = 115, ENOTCONN = 107, EBADF = 9, EINVAL = 22, EPIPE = 32;
  const UV_EOF = -4095;
  const O_NONBLOCK = 0o4000, O_CLOEXEC = 0o2000000;
  const F_GETFL = 3, F_SETFL = 4;
  const AF_UNIX = 1, AF_INET = 2, AF_INET6 = 10;
  const SOCK_STREAM = 1, SOCK_NONBLOCK = 0o4000, SOCK_CLOEXEC = 0o2000000;
  const SOL_SOCKET = 1, SO_REUSEADDR = 2, SO_ERROR = 4, SO_KEEPALIVE = 9, SO_LINGER = 13;
  const IPPROTO_TCP = 6, TCP_NODELAY = 1, TCP_KEEPIDLE = 4, IPPROTO_IPV6 = 41, IPV6_V6ONLY = 26;
  const ADDR = S + 1024;	// a socket address in the scratch block, and its length just before
  const ADDRLEN = S + 1016;

  const state = new Int32Array(4);	// kReadBytesOrError, kArrayBufferOffset, kBytesWritten, kLastWriteWasAsync
  const kReadBytesOrError = 0, kArrayBufferOffset = 1, kBytesWritten = 2, kLastWriteWasAsync = 3;

  class WriteWrap extends hn.AsyncWrap {
    constructor() {
      super("WRITEWRAP");
    }
  }
  class ShutdownWrap extends hn.AsyncWrap {
    constructor() {
      super("SHUTDOWNWRAP");
    }
  }

  hn.setNonBlocking = (fd, on) => {
    const flags = sys3(SYS.fcntl, fd, F_GETFL, 0);
    if (flags < 0) {
      return flags;
    }
    const wanted = on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    return wanted === flags ? 0 : sys3(SYS.fcntl, fd, F_SETFL, wanted);
  };

  // --- Addresses.
  const parse4 = (text) => {
    const parts = String(text).split(".");
    if (parts.length !== 4) {
      return null;
    }
    const out = [];
    for (const part of parts) {
      if (!/^\d{1,3}$/.test(part) || Number(part) > 255) {
        return null;
      }
      out.push(Number(part));
    }
    return out;
  };
  const parse6 = (text) => {
    text = String(text);
    const percent = text.indexOf("%");
    if (percent >= 0) {
      text = text.slice(0, percent);
    }
    let tail = null;
    const lastColon = text.lastIndexOf(":");
    if (text.includes(".")) {
      tail = parse4(text.slice(lastColon + 1));
      if (!tail) {
        return null;
      }
      text = text.slice(0, lastColon + 1) + "0:0";
    }
    const halves = text.split("::");
    if (halves.length > 2) {
      return null;
    }
    const groups = (s) => (s === "" ? [] : s.split(":"));
    const head = groups(halves[0]), rest = halves.length === 2 ? groups(halves[1]) : [];
    const missing = 8 - head.length - rest.length;
    if (halves.length === 2 ? missing < 1 : missing !== 0) {
      return null;
    }
    const words = [...head, ...new Array(halves.length === 2 ? missing : 0).fill("0"), ...rest];
    const out = [];
    for (const word of words) {
      if (!/^[0-9a-fA-F]{1,4}$/.test(word)) {
        return null;
      }
      const n = parseInt(word, 16);
      out.push(n >> 8, n & 255);
    }
    if (tail) {
      out.splice(12, 4, ...tail);
    }
    return out;
  };
  const format6 = (b) => {
    const words = [];
    for (let i = 0; i < 16; i += 2) {
      words.push((b[i] << 8) | b[i + 1]);
    }
    // The longest run of zero words becomes "::".
    let best = -1, bestLength = 0;
    for (let i = 0; i < 8;) {
      if (words[i] !== 0) {
        i++;
        continue;
      }
      let j = i;
      while (j < 8 && words[j] === 0) {
        j++;
      }
      if (j - i > bestLength && j - i > 1) {
        best = i;
        bestLength = j - i;
      }
      i = j;
    }
    const hex = words.map((w) => w.toString(16));
    if (best < 0) {
      return hex.join(":");
    }
    return hex.slice(0, best).join(":") + "::" + hex.slice(best + bestLength).join(":");
  };
  // Write a socket address at ADDR; returns its length, or a negative error.
  const putAddress = (family, address, port) => {
    const m = u8(), v = dv();
    m.fill(0, ADDR, ADDR + 128);
    v.setUint16(ADDR, family, true);
    if (family === AF_UNIX) {
      const raw = typeof address === "string" ? hn.encoder.encode(address) : address;
      if (raw.length > 107) {
        return -36;
      }
      m.set(raw, ADDR + 2);
      return 2 + raw.length + 1;
    }
    v.setUint16(ADDR + 2, port, false);
    if (family === AF_INET) {
      const b = parse4(address);
      if (!b) {
        return -EINVAL;
      }
      m.set(b, ADDR + 4);
      return 16;
    }
    const b = parse6(address);
    if (!b) {
      return -EINVAL;
    }
    m.set(b, ADDR + 8);
    return 28;
  };
  // What a socket address at ADDR says, into `out` (lib/net.js: getsockname).
  const readAddress = (out) => {
    const m = u8(), v = dv();
    const family = v.getUint16(ADDR, true);
    if (family === AF_INET) {
      out.address = m[ADDR + 4] + "." + m[ADDR + 5] + "." + m[ADDR + 6] + "." + m[ADDR + 7];
      out.family = "IPv4";
      out.port = v.getUint16(ADDR + 2, false);
    } else if (family === AF_INET6) {
      out.address = format6(m.subarray(ADDR + 8, ADDR + 24));
      out.family = "IPv6";
      out.port = v.getUint16(ADDR + 2, false);
    }
    return out;
  };
  hn.net = { parse4, parse6, format6 };

  // --- A stream on a descriptor.
  class Stream extends hn.HandleWrap {
    constructor(provider) {
      super(provider);
      this.__fd = -1;
      this.__watcher = null;
      this.__reading = false;
      this.__eof = false;
      this.__writes = [];	// what has not been written yet: { data, at, request }
      this.__connect = null;
      this.__listening = false;
      this.__shutdown = null;
      this.__ours = false;	// the descriptor is not shared with anyone: it may be non-blocking
      this.__userBuffer = null;
      this.bytesRead = 0;
      this.bytesWritten = 0;
      this.writeQueueSize = 0;
    }
    get fd() {
      return this.__fd;
    }
    get isStreamBase() {
      return true;
    }
    get _externalStream() {
      return this;
    }
    // Take a descriptor: watch it.
    __adopt(fd, ours) {
      this.__fd = fd;
      this.__ours = !!ours;
      if (ours) {
        hn.setNonBlocking(fd, true);
      }
      this.__watcher = loop.watch(fd, 0, (events) => this.__events(events));
      if (this.__watcher.error) {
        // Not something the kernel can watch (a plain file): treated as always ready.
        this.__watcher = null;
      }
      return 0;
    }
    // Stop taking what arrives for a while (whoever it is passed on to has no room), without ceasing to be
    // a stream that is being read.
    __pause(on) {
      if (!!this.__paused !== on) {
        this.__paused = on;
        if (!this.__closed) {
          this.__interest();
        }
      }
    }
    __interest() {
      const mask = ((this.__reading && !this.__paused) || this.__listening ? EPOLLIN : 0) |
        (this.__writes.length || this.__connect ? EPOLLOUT : 0);
      if (this.__watcher) {
        this.__watcher.set(mask);
      }
      this.__setActive(mask !== 0 || this.__shutdown !== null || (this.__reading && !!this.__paused));
    }
    __events(events) {
      if (this.__closed) {
        return;
      }
      if (this.__connect) {
        this.__connected();
        return;
      }
      if (this.__listening) {
        this.__accept();
        return;
      }
      if (this.__writes.length && (events & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
        this.__flush();
      }
      if (this.__reading && !this.__paused && (events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP))) {
        this.__read();
      }
    }
    __read() {
      const user = this.__userBuffer;
      const want = user ? Math.min(user.length, SCRATCH) : SCRATCH;
      const n = sys3(SYS.read, this.__fd, S, want);
      if (n === -EAGAIN || n === -EINTR) {
        return;
      }
      if (n > 0 && this.__consumer) {
        // (An HTTP parser reads this stream itself: binding/http.js.)
        this.bytesRead += n;
        const bytes = hn.copy(S, n);
        hn.makeCallback(this, () => this.__consumer(bytes));
        return;
      }
      if (n > 0) {
        this.bytesRead += n;
        let buffer;
        if (user) {
          user.set(u8().subarray(S, S + n));
        } else {
          buffer = hn.copy(S, n).buffer;
        }
        state[kReadBytesOrError] = n;
        state[kArrayBufferOffset] = 0;
        hn.makeCallback(this, () => {
          const next = this.onread(buffer);
          if (user && next instanceof Uint8Array) {
            this.__userBuffer = next;
          }
        });
        return;
      }
      // The end, or an error: nothing more will come.
      if (hn.debug) {
        hn.writeAll(2, "hostnode: read on " + this.__fd + " gave " + n + "\n");
      }
      this.__reading = false;
      this.__eof = true;
      this.__interest();
      state[kReadBytesOrError] = n === 0 ? UV_EOF : n;
      state[kArrayBufferOffset] = 0;
      hn.makeCallback(this, () => this.onread(undefined));
    }
    readStart() {
      if (this.__fd < 0) {
        return -EBADF;
      }
      this.__reading = true;
      this.__interest();
      if (!this.__watcher) {
        loop.enqueue(() => {
          if (this.__reading && !this.__closed) {
            this.__read();
          }
        });
      }
      return 0;
    }
    readStop() {
      this.__reading = false;
      this.__interest();
      return 0;
    }
    useUserBuffer(buffer) {
      this.__userBuffer = buffer;
    }
    setBlocking(blocking) {
      // (Only what is ours alone is ever made non-blocking; see the top of this file.)
      return 0;
    }
    // Write as much as the kernel takes now; the rest waits for it to have room.
    __write(request, bytes) {
      state[kBytesWritten] = bytes.length;
      state[kLastWriteWasAsync] = 0;
      if (this.__fd < 0) {
        return -EBADF;
      }
      let done = 0;
      if (this.__writes.length === 0) {
        while (done < bytes.length) {
          const part = Math.min(bytes.length - done, SCRATCH);
          u8().set(bytes.subarray(done, done + part), S);
          const n = sys3(SYS.write, this.__fd, S, part);
          if (n === -EINTR) {
            continue;
          }
          if (n === -EAGAIN) {
            if (!this.__watcher) {
              return n;
            }
            break;
          }
          if (n < 0) {
            return n;
          }
          done += n;
        }
      }
      this.bytesWritten += done;
      if (done === bytes.length) {
        return 0;
      }
      this.__writes.push({ data: bytes, at: done, request });
      this.writeQueueSize += bytes.length - done;
      state[kLastWriteWasAsync] = 1;
      this.__interest();
      return 0;
    }
    __flush() {
      while (this.__writes.length) {
        const w = this.__writes[0];
        let failed = 0;
        while (w.at < w.data.length) {
          const part = Math.min(w.data.length - w.at, SCRATCH);
          u8().set(w.data.subarray(w.at, w.at + part), S);
          const n = sys3(SYS.write, this.__fd, S, part);
          if (n === -EINTR) {
            continue;
          }
          if (n === -EAGAIN) {
            this.__interest();
            return;
          }
          if (n < 0) {
            failed = n;
            break;
          }
          w.at += n;
          this.bytesWritten += n;
          this.writeQueueSize -= n;
        }
        this.__writes.shift();
        if (failed) {
          this.writeQueueSize -= w.data.length - w.at;
        }
        hn.makeCallback(w.request, () => hn.oncomplete(w.request, failed));
        if (this.__closed) {
          return;
        }
      }
      this.__interest();
      if (this.__shutdown) {
        this.__finishShutdown();
      }
    }
    writeBuffer(request, buffer) {
      return this.__write(request, buffer instanceof Uint8Array ? buffer :
        new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength));
    }
    writeUtf8String(request, string) {
      return this.__write(request, hn.encoder.encode(string));
    }
    writeAsciiString(request, string) {
      return this.__write(request, hn.encodeString(string, 0));
    }
    writeLatin1String(request, string) {
      return this.__write(request, hn.encodeString(string, 4));
    }
    writeUcs2String(request, string) {
      return this.__write(request, hn.encodeString(string, 3));
    }
    writev(request, chunks, allBuffers) {
      const parts = [];
      let total = 0;
      if (allBuffers) {
        for (const chunk of chunks) {
          parts.push(chunk);
          total += chunk.length;
        }
      } else {
        const names = shapes.string_decoder.encodings;
        for (let i = 0; i < chunks.length; i += 2) {
          let chunk = chunks[i];
          if (typeof chunk === "string") {
            chunk = hn.encodeString(chunk, Math.max(0, names.indexOf(hn.normalizeEncoding(chunks[i + 1]))));
          }
          parts.push(chunk);
          total += chunk.length;
        }
      }
      const all = new Uint8Array(total);
      let at = 0;
      for (const part of parts) {
        all.set(part, at);
        at += part.length;
      }
      return this.__write(request, all);
    }
    shutdown(request) {
      if (this.__fd < 0) {
        return -ENOTCONN;
      }
      this.__shutdown = request;
      this.__interest();
      if (this.__writes.length === 0) {
        this.__finishShutdown();
      }
      return 0;
    }
    __finishShutdown() {
      const request = this.__shutdown;
      this.__shutdown = null;
      let r = sys2(SYS.shutdown, this.__fd, 1);	// SHUT_WR
      if (r === -88 || r === -ENOTCONN) {
        // Not a socket (a pipe, a terminal): there is no half to close.
        r = 0;
      }
      loop.refs++;
      loop.enqueue(() => {
        loop.refs--;
        this.__interest();
        hn.makeCallback(request, () => hn.oncomplete(request, r));
      });
    }
    __close() {
      if (this.__watcher) {
        this.__watcher.close();
        this.__watcher = null;
      }
      this.__reading = false;
      this.__listening = false;
      const pending = this.__writes;
      this.__writes = [];
      for (const w of pending) {
        loop.enqueue(() => hn.makeCallback(w.request, () => hn.oncomplete(w.request, -125)));	// ECANCELED
      }
      if (this.__connect) {
        const request = this.__connect;
        this.__connect = null;
        loop.enqueue(() => hn.makeCallback(request, () => hn.oncomplete(request, -125, this, request, false, false)));
      }
      if (this.__fd > 2 || (this.__fd >= 0 && this.__ours)) {
        sys1(SYS.close, this.__fd);
      }
      this.__fd = -1;
    }

    // --- Sockets: connecting, listening.
    __socket(family) {
      const fd = sys3(SYS.socket, family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return fd;
      }
      this.__adopt(fd, true);
      return 0;
    }
    __doConnect(request, family, address, port) {
      if (this.__fd < 0) {
        const r = this.__socket(family);
        if (r < 0) {
          return r;
        }
      }
      const length = putAddress(family, address, port);
      if (length < 0) {
        return length;
      }
      let r;
      do {
        r = sys3(SYS.connect, this.__fd, ADDR, length);
      } while (r === -EINTR);
      if (r < 0 && r !== -EINPROGRESS) {
        // libuv reports a refusal that is known at once (a local address) later, like the others.
        if (r !== -111) {
          return r;
        }
        this.__connect = request;
        loop.refs++;
        loop.enqueue(() => {
          loop.refs--;
          if (this.__connect === request) {
            this.__connect = null;
            hn.makeCallback(request, () => hn.oncomplete(request, r, this, request, false, false));
          }
        });
        return 0;
      }
      this.__connect = request;
      this.__interest();
      if (r === 0 && !this.__watcher) {
        this.__connected();
      }
      return 0;
    }
    __connected() {
      const request = this.__connect;
      this.__connect = null;
      const v = dv();
      v.setUint32(ADDRLEN, 4, true);
      let status = sys5(SYS.getsockopt, this.__fd, SOL_SOCKET, SO_ERROR, ADDR, ADDRLEN);
      if (status === 0) {
        status = -v.getInt32(ADDR, true);
      }
      this.__interest();
      hn.makeCallback(request, () => hn.oncomplete(request, status, this, request, status === 0, status === 0));
    }
    __doBind(family, address, port, flags) {
      if (this.__fd < 0) {
        const r = this.__socket(family);
        if (r < 0) {
          return r;
        }
      }
      const v = dv();
      if (family !== AF_UNIX) {
        v.setInt32(S, 1, true);
        sys5(SYS.setsockopt, this.__fd, SOL_SOCKET, SO_REUSEADDR, S, 4);
        if (family === AF_INET6) {
          v.setInt32(S, flags & 1 ? 1 : 0, true);	// UV_TCP_IPV6ONLY
          sys5(SYS.setsockopt, this.__fd, IPPROTO_IPV6, IPV6_V6ONLY, S, 4);
        }
      }
      const length = putAddress(family, address, port);
      if (length < 0) {
        return length;
      }
      return sys3(SYS.bind, this.__fd, ADDR, length);
    }
    listen(backlog) {
      if (this.__fd < 0) {
        return -EINVAL;
      }
      const r = sys2(SYS.listen, this.__fd, backlog);
      if (r < 0) {
        return r;
      }
      this.__listening = true;
      this.__interest();
      return 0;
    }
    __accept() {
      const fd = sys4(SYS.accept4, this.__fd, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd === -EAGAIN || fd === -EINTR || fd === -103) {	// ECONNABORTED
        return;
      }
      if (fd < 0) {
        hn.makeCallback(this, () => this.onconnection(fd, undefined));
        return;
      }
      const client = this.__newConnection();
      client.__adopt(fd, true);
      hn.makeCallback(this, () => this.onconnection(0, client));
    }
    open(fd) {
      // A descriptor of the program's: from here on it is this handle's, as libuv has it.
      return this.__adopt(fd, fd > 2);
    }
  }
  for (const name of ["__fd", "__watcher", "__reading", "__eof", "__writes", "__connect", "__listening",
                      "__shutdown", "__ours", "__userBuffer", "__consumer", "__paused"]) {
    Object.defineProperty(Stream.prototype, name, { writable: true, enumerable: false, value: null });
  }
  hn.Stream = Stream;

  hn.binding("stream_wrap", () => ({
    WriteWrap, ShutdownWrap,
    kReadBytesOrError, kArrayBufferOffset, kBytesWritten, kLastWriteWasAsync,
    streamBaseState: state,
    LibuvStreamWrap: Stream,
  }));

  // --- Terminals (src/tty_wrap.cc).
  const TCGETS = 0x5401, TCSETSW = 0x5403, TIOCGWINSZ = 0x5413;
  const TERMIOS = S + 2048;
  const isTTY = (fd) => sys3(SYS.ioctl, fd, TCGETS, TERMIOS) === 0;
  // The settings of terminals as we found them, by descriptor: put back when the program ends.
  const original = new Map();
  hn.atExit.push(() => {
    for (const [fd, saved] of original) {
      u8().set(saved, TERMIOS);
      sys3(SYS.ioctl, fd, TCSETSW, TERMIOS);
    }
  });
  hn.binding("tty_wrap", () => {
    class TTY extends Stream {
      constructor(fd, ctx) {
        super("TTYWRAP");
        if (!isTTY(fd)) {
          hn.fail(ctx, -EINVAL, "uv_tty_init");
          return;
        }
        this.__adopt(fd, false);
      }
      getWindowSize(size) {
        const r = sys3(SYS.ioctl, this.__fd, TIOCGWINSZ, S);
        if (r < 0) {
          return r;
        }
        size[0] = dv().getUint16(S + 2, true);
        size[1] = dv().getUint16(S, true);
        return 0;
      }
      // libuv: uv_tty_set_mode. Raw: every key at once, as it is, no echo, no signals from keys.
      setRawMode(raw) {
        const fd = this.__fd;
        let r = sys3(SYS.ioctl, fd, TCGETS, TERMIOS);
        if (r < 0) {
          return r;
        }
        if (!original.has(fd)) {
          original.set(fd, hn.copy(TERMIOS, 36));
        }
        if (raw) {
          const v = dv();
          v.setUint32(TERMIOS, v.getUint32(TERMIOS, true) & ~(0o2 | 0o400 | 0o20 | 0o40 | 0o2000), true);	// BRKINT ICRNL INPCK ISTRIP IXON
          v.setUint32(TERMIOS + 4, v.getUint32(TERMIOS + 4, true) | 0o4, true);	// ONLCR
          v.setUint32(TERMIOS + 8, v.getUint32(TERMIOS + 8, true) | 0o60, true);	// CS8
          v.setUint32(TERMIOS + 12, v.getUint32(TERMIOS + 12, true) & ~(0o10 | 0o2 | 0o100000 | 0o1), true);	// ECHO ICANON IEXTEN ISIG
          u8()[TERMIOS + 17 + 6] = 1;	// VMIN
          u8()[TERMIOS + 17 + 5] = 0;	// VTIME
        } else {
          u8().set(original.get(fd), TERMIOS);
        }
        r = sys3(SYS.ioctl, fd, TCSETSW, TERMIOS);
        return r < 0 ? r : 0;
      }
    }
    return { TTY, isTTY };
  });

  // --- Pipes and local sockets (src/pipe_wrap.cc).
  hn.binding("pipe_wrap", () => {
    class PipeConnectWrap extends hn.AsyncWrap {
      constructor() {
        super("PIPECONNECTWRAP");
      }
    }
    class Pipe extends Stream {
      constructor(type) {
        super(type === 1 ? "PIPESERVERWRAP" : "PIPEWRAP");
        this.__type = type;
      }
      __newConnection() {
        return new Pipe(0);
      }
      bind(name) {
        return this.__doBind(AF_UNIX, name, 0, 0);
      }
      connect(request, name) {
        return this.__doConnect(request, AF_UNIX, name, 0);
      }
      fchmod(mode) {
        // UV_READABLE 1, UV_WRITABLE 2
        const bits = (mode & 1 ? 0o444 : 0) | (mode & 2 ? 0o222 : 0);
        return sys2(SYS.fchmod, this.__fd, bits);
      }
    }
    Object.defineProperty(Pipe.prototype, "__type", { writable: true, enumerable: false, value: 0 });
    hn.Pipe = Pipe;
    return { Pipe, PipeConnectWrap, constants: shapes.pipe_wrap.constants };
  });

  // --- TCP (src/tcp_wrap.cc).
  hn.binding("tcp_wrap", () => {
    class TCPConnectWrap extends hn.AsyncWrap {
      constructor() {
        super("TCPCONNECTWRAP");
      }
    }
    class TCP extends Stream {
      constructor(type) {
        super(type === 1 ? "TCPSERVERWRAP" : "TCPWRAP");
      }
      __newConnection() {
        return new TCP(0);
      }
      bind(address, port) {
        return this.__doBind(AF_INET, address, port, 0);
      }
      bind6(address, port, flags) {
        return this.__doBind(AF_INET6, address, port, flags || 0);
      }
      connect(request, address, port) {
        return this.__doConnect(request, AF_INET, address, port);
      }
      connect6(request, address, port) {
        return this.__doConnect(request, AF_INET6, address, port);
      }
      getsockname(out) {
        dv().setUint32(ADDRLEN, 128, true);
        const r = sys3(SYS.getsockname, this.__fd, ADDR, ADDRLEN);
        if (r === 0) {
          readAddress(out);
        }
        return r;
      }
      getpeername(out) {
        dv().setUint32(ADDRLEN, 128, true);
        const r = sys3(SYS.getpeername, this.__fd, ADDR, ADDRLEN);
        if (r === 0) {
          readAddress(out);
        }
        return r;
      }
      setNoDelay(enable) {
        dv().setInt32(S, enable ? 1 : 0, true);
        return sys5(SYS.setsockopt, this.__fd, IPPROTO_TCP, TCP_NODELAY, S, 4);
      }
      setKeepAlive(enable, delay) {
        dv().setInt32(S, enable ? 1 : 0, true);
        const r = sys5(SYS.setsockopt, this.__fd, SOL_SOCKET, SO_KEEPALIVE, S, 4);
        if (r === 0 && enable && delay > 0) {
          dv().setInt32(S, delay, true);
          sys5(SYS.setsockopt, this.__fd, IPPROTO_TCP, TCP_KEEPIDLE, S, 4);
        }
        return r;
      }
      // Close at once, telling the other side so (no orderly end).
      reset(callback) {
        if (this.__fd >= 0) {
          dv().setInt32(S, 1, true);
          dv().setInt32(S + 4, 0, true);
          sys5(SYS.setsockopt, this.__fd, SOL_SOCKET, SO_LINGER, S, 8);
        }
        this.close(callback);
        return 0;
      }
    }
    hn.TCP = TCP;
    return { TCP, TCPConnectWrap, constants: shapes.tcp_wrap.constants };
  });

  // --- Names to addresses (src/cares_wrap.cc: getaddrinfo). The C library asks the name servers of the
  // guest system; the program waits while it does.
  hn.binding("cares_wrap", () => {
    const c = shapes.cares_wrap;
    class GetAddrInfoReqWrap extends hn.AsyncWrap {
      constructor() {
        super("GETADDRINFOREQWRAP");
      }
    }
    class GetNameInfoReqWrap extends hn.AsyncWrap {
      constructor() {
        super("GETNAMEINFOREQWRAP");
      }
    }
    class QueryReqWrap extends hn.AsyncWrap {
      constructor() {
        super("QUERYWRAP");
      }
    }
    // The C library's EAI_ numbers, to the names libuv has for them.
    const eai = { 1: "EAI_BADFLAGS", 2: "EAI_NONAME", 3: "EAI_AGAIN", 4: "EAI_FAIL", 5: "EAI_NODATA", 6: "EAI_FAMILY",
                  7: "EAI_SOCKTYPE", 8: "EAI_SERVICE", 10: "EAI_MEMORY", 11: "EAI_SYSTEM", 12: "EAI_OVERFLOW" };
    const uvCode = (name) => {
      for (const [code, [n]] of data.uvErrors) {
        if (n === name) {
          return code;
        }
      }
      return -3008;
    };
    // A resolver that asks name servers for records itself (dns.resolve*): only its housekeeping is here.
    // Names are looked up by the C library (getaddrinfo below), which is what dns.lookup and every
    // connection by name use.
    class ChannelWrap extends hn.AsyncWrap {
      constructor() {
        super("DNSCHANNEL");
        this.__servers = null;
      }
      getServers() {
        if (this.__servers === null) {
          this.__servers = [];
          for (const line of (hn.readText("/etc/resolv.conf") || "").split("\n")) {
            const m = /^\s*nameserver\s+(\S+)/.exec(line);
            if (m) {
              this.__servers.push([m[1], 53]);
            }
          }
        }
        return this.__servers;
      }
      setServers(servers) {
        this.__servers = servers.map((s) => [s[1], s[2] || 53]);
        return 0;
      }
      setLocalAddress() {}
      cancel() {}
    }
    for (const name of data.bindings.cares_wrap.ChannelWrap.prototype) {
      if (!(name in ChannelWrap.prototype)) {
        ChannelWrap.prototype[name] = () => 5;	// ARES_ENOTIMP
      }
    }
    Object.defineProperty(ChannelWrap.prototype, "__servers", { writable: true, enumerable: false, value: null });
    return hn.complete("cares_wrap", {
      GetAddrInfoReqWrap, GetNameInfoReqWrap, QueryReqWrap, ChannelWrap,
      getaddrinfo(request, hostname, family, hints, order) {
        const name = hn.cstringNew(String(hostname));
        const out = hn.alloc(17 * 32);
        const n = Number(hn.C.hn_lookup(BigInt(name), family === 4 ? AF_INET : family === 6 ? AF_INET6 : 0, BigInt(out), 32));
        const addresses = [];
        const m = u8();
        for (let i = 0; i < n; i++) {
          const at = out + 17 * i;
          addresses.push(m[at] === 4 ? m[at + 1] + "." + m[at + 2] + "." + m[at + 3] + "." + m[at + 4] :
            format6(m.subarray(at + 1, at + 17)));
        }
        hn.free(name);
        hn.free(out);
        if (order === c.DNS_ORDER_IPV4_FIRST) {
          addresses.sort((a, b) => (a.includes(":") ? 1 : 0) - (b.includes(":") ? 1 : 0));
        } else if (order === c.DNS_ORDER_IPV6_FIRST) {
          addresses.sort((a, b) => (a.includes(":") ? 0 : 1) - (b.includes(":") ? 0 : 1));
        }
        const status = n < 0 ? uvCode(eai[-n] || "EAI_FAIL") : n === 0 ? uvCode("EAI_NODATA") : 0;
        loop.refs++;
        loop.enqueue(() => {
          loop.refs--;
          hn.makeCallback(request, () => hn.oncomplete(request, status, status === 0 ? addresses : undefined));
        });
        return 0;
      },
      canonicalizeIP(ip) {
        const b4 = parse4(ip);
        if (b4) {
          return b4.join(".");
        }
        const b6 = parse6(ip);
        return b6 ? format6(b6) : undefined;
      },
      strerror: (code) => (code === 5 ? "DNS server does not implement requested operation" : "DNS error " + code),
    });
  });

  hn.binding("js_stream", () => hn.complete("js_stream", {}));
  hn.binding("stream_pipe", () => hn.complete("stream_pipe", {}));
  hn.binding("udp_wrap", () => hn.complete("udp_wrap", {}));
})
