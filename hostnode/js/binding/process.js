// Other programs (src/process_wrap.cc, src/spawn_sync.cc, on libuv's
// uv_spawn): started by the C library under us (hostnode.c: hn_spawn), which
// can do what JavaScript cannot, namely be the child between clone and exec.
// They are programs of the guest system like any other; what connects us to
// them are pipes (socket pairs, as libuv makes them) and wait4.
(function (hn) {
  "use strict";

  const { loop, data, SYS, sys1, sys2, sys3, sys4, sys5, dv, u8, S, SCRATCH } = hn;
  const AF_UNIX = 1, SOCK_STREAM = 1, SOCK_CLOEXEC = 0o2000000;
  const SIGKILL = 9;
  const WNOHANG = 1;
  const EINTR = 4, EAGAIN = 11, ECHILD = 10;
  const UV_ETIMEDOUT = -110, UV_ENOBUFS = -105;
  const POLLIN = 1, POLLOUT = 4, POLLERR = 8, POLLHUP = 16;

  const signalNames = {};
  for (const [name, number] of Object.entries(data.bindings.constants.os.signals)) {
    if (!(number in signalNames)) {
      signalNames[number] = name;
    }
  }

  // Lay out what hn_spawn takes (struct hn_spawn in hostnode.c) in one block of memory; the caller frees it.
  const prepare = (options, fds) => {
    const strings = [];
    const add = (s) => {
      const raw = hn.encoder.encode(String(s));
      strings.push(raw);
      return raw.length + 1;
    };
    let size = add(options.file);
    const args = options.args && options.args.length ? options.args : [options.file];
    for (const arg of args) {
      size += add(arg);
    }
    const env = options.envPairs || [...hn.env].map(([k, v]) => k + "=" + v);
    for (const pair of env) {
      size += add(pair);
    }
    if (options.cwd) {
      size += add(options.cwd);
    }
    const header = 64;
    const pointers = 8 * (args.length + 1 + env.length + 1);
    const fdBytes = 4 * fds.length;
    const block = hn.alloc(header + pointers + fdBytes + size + 8);
    const v = dv(), m = u8();
    let stringAt = block + header + pointers + fdBytes;
    let next = 0;
    const put = () => {
      const raw = strings[next++];
      const at = stringAt;
      m.set(raw, at);
      m[at + raw.length] = 0;
      stringAt += raw.length + 1;
      return at;
    };
    v.setBigUint64(block, BigInt(put()), true);
    let p = block + header;
    v.setBigUint64(block + 8, BigInt(p), true);
    for (let i = 0; i < args.length; i++, p += 8) {
      v.setBigUint64(p, BigInt(put()), true);
    }
    v.setBigUint64(p, 0n, true);
    p += 8;
    v.setBigUint64(block + 16, BigInt(p), true);
    for (let i = 0; i < env.length; i++, p += 8) {
      v.setBigUint64(p, BigInt(put()), true);
    }
    v.setBigUint64(p, 0n, true);
    p += 8;
    v.setBigUint64(block + 24, options.cwd ? BigInt(put()) : 0n, true);
    v.setBigUint64(block + 32, BigInt(p), true);
    for (let i = 0; i < fds.length; i++) {
      v.setInt32(p + 4 * i, fds[i], true);
    }
    v.setInt32(block + 40, fds.length, true);
    let flags = options.detached ? 1 : 0;
    if (typeof options.uid === "number") {
      flags |= 2;
      v.setInt32(block + 48, options.uid, true);
    }
    if (typeof options.gid === "number") {
      flags |= 4;
      v.setInt32(block + 52, options.gid, true);
    }
    v.setInt32(block + 44, flags, true);
    v.setInt32(block + 56, 0, true);
    return block;
  };
  const socketpair = () => {
    const r = sys4(SYS.socketpair, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, S);
    if (r < 0) {
      return r;
    }
    return [dv().getInt32(S, true), dv().getInt32(S + 4, true)];
  };
  // Start the program: the child's descriptors are fds (in the parent: -1 for none). Returns the process
  // number or a negative error.
  const start = (options, fds) => {
    const block = prepare(options, fds);
    const pid = Number(hn.C.hn_spawn(BigInt(block)));
    hn.free(block);
    return pid;
  };
  hn.spawn = start;
  const status = (word) => {
    const signal = word & 0x7f;
    return signal ? { code: 0, signal } : { code: (word >> 8) & 0xff, signal: 0 };
  };
  const wait = (pid, flags) => {
    for (;;) {
      const r = sys4(SYS.wait4, pid, S, flags, 0);
      if (r === -EINTR) {
        continue;
      }
      return r > 0 ? status(dv().getInt32(S, true)) : r;
    }
  };

  // A descriptor for a child: the kernel makes it readable when the child has ended.
  const pidfd = (pid) => sys2(SYS.pidfd_open, pid, 0);

  hn.binding("process_wrap", () => {
    class Process extends hn.HandleWrap {
      constructor() {
        super("PROCESSWRAP");
        this.pid = 0;
      }
      spawn(options) {
        const stdio = options.stdio || [];
        const fds = [];
        const ours = [];	// [handle, our end] for each pipe
        const theirs = [];
        for (const entry of stdio) {
          if (entry.type === "ignore") {
            fds.push(-1);
          } else if (entry.type === "pipe" || entry.type === "overlapped") {
            const pair = socketpair();
            if (typeof pair === "number") {
              for (const fd of [...theirs, ...ours.map((o) => o[1])]) {
                sys1(SYS.close, fd);
              }
              return pair;
            }
            ours.push([entry.handle, pair[0]]);
            theirs.push(pair[1]);
            fds.push(pair[1]);
          } else if (entry.type === "wrap") {
            fds.push(entry.handle.fd);
          } else {
            fds.push(entry.fd);
          }
        }
        const pid = start(options, fds);
        for (const fd of theirs) {
          sys1(SYS.close, fd);
        }
        if (pid < 0) {
          for (const [, fd] of ours) {
            sys1(SYS.close, fd);
          }
          return pid;
        }
        for (const [handle, fd] of ours) {
          handle.__adopt(fd, true);
        }
        this.pid = pid;
        const fd = pidfd(pid);
        if (fd < 0) {
          // Without it we would never learn of its end: better no child than one nobody waits for.
          sys2(SYS.kill, pid, SIGKILL);
          wait(pid, 0);
          for (const [handle] of ours) {
            handle.close();
          }
          this.pid = 0;
          return fd;
        }
        this.__setActive(true);
        this.__watcher = loop.watch(fd, 1, () => {
          const ended = wait(pid, WNOHANG);
          if (ended === 0) {
            return;
          }
          this.__watcher.close();
          this.__watcher = null;
          sys1(SYS.close, fd);
          this.__setActive(false);
          const result = typeof ended === "object" ? ended : { code: 0, signal: 0 };
          hn.makeCallback(this, () => this.onexit(result.code, result.signal ? signalNames[result.signal] || "" : ""));
        });
        return 0;
      }
      kill(signal) {
        if (!this.pid) {
          return -3;	// ESRCH
        }
        return sys2(SYS.kill, this.pid, signal);
      }
      __close() {
        // (A child that still runs goes on running: nobody waits for it any more.)
        if (this.__watcher) {
          const fd = this.__watcher.fd;
          this.__watcher.close();
          this.__watcher = null;
          sys1(SYS.close, fd);
        }
      }
    }
    Object.defineProperty(Process.prototype, "__watcher", { writable: true, enumerable: false, value: null });
    return { Process };
  });

  // --- A program run to its end while we wait (child_process.spawnSync, execSync).
  hn.binding("spawn_sync", () => ({
    spawn(options) {
      const { Buffer } = hn.requireBuiltin("buffer");
      const stdio = options.stdio || [];
      const fds = [];
      const pipes = [];	// { index, fd (ours), input, sent, chunks, size, open }
      const theirs = [];
      const closeAll = () => {
        for (const fd of theirs) {
          sys1(SYS.close, fd);
        }
        for (const pipe of pipes) {
          if (pipe.open) {
            sys1(SYS.close, pipe.fd);
            pipe.open = false;
          }
        }
      };
      for (let i = 0; i < stdio.length; i++) {
        const entry = stdio[i];
        if (entry.type === "ignore") {
          fds.push(-1);
        } else if (entry.type === "pipe") {
          const pair = socketpair();
          if (typeof pair === "number") {
            closeAll();
            return { error: pair, status: null, signal: null, output: null, pid: 0 };
          }
          hn.setNonBlocking(pair[0], true);
          let input = null;
          if (entry.readable && entry.input !== undefined && entry.input !== null) {
            input = entry.input instanceof Uint8Array ? entry.input :
              new Uint8Array(entry.input.buffer, entry.input.byteOffset, entry.input.byteLength);
          }
          pipes.push({ index: i, fd: pair[0], input, sent: 0, collect: !!entry.writable, chunks: [], size: 0,
                       open: true, writing: !!entry.readable });
          theirs.push(pair[1]);
          fds.push(pair[1]);
        } else {
          fds.push(entry.fd);
        }
      }
      const pid = start(options, fds);
      for (const fd of theirs.splice(0)) {
        sys1(SYS.close, fd);
      }
      if (pid < 0) {
        closeAll();
        return { error: pid, status: null, signal: null, output: null, pid: 0 };
      }
      const childFd = pidfd(pid);
      const began = hn.web.performance.now();
      const timeout = options.timeout > 0 ? options.timeout : 0;
      const maxBuffer = typeof options.maxBuffer === "number" ? options.maxBuffer : Infinity;
      const killSignal = options.killSignal || 15;
      let error = 0;
      let ended = null;
      let killed = false;
      const kill = (why) => {
        if (!killed) {
          killed = true;
          error = why;
          sys2(SYS.kill, pid, killSignal);
        }
      };
      // The descriptors to wait on are written behind where data is read to.
      const POLL = S + SCRATCH - 8 * 16;
      const finishWriting = (pipe) => {
        pipe.writing = false;
        if (pipe.collect) {
          sys2(SYS.shutdown, pipe.fd, 1);
        } else {
          sys1(SYS.close, pipe.fd);
          pipe.open = false;
        }
      };
      for (;;) {
        if (ended === null) {
          const r = wait(pid, WNOHANG);
          if (r !== 0) {
            ended = typeof r === "object" ? r : { code: 0, signal: 0 };
          }
        }
        const live = pipes.filter((pipe) => pipe.open);
        if (ended !== null && live.every((pipe) => !pipe.collect)) {
          break;
        }
        if (ended !== null && killed) {
          break;
        }
        const v = dv();
        let n = 0;
        if (ended === null && childFd >= 0) {
          v.setInt32(POLL, childFd, true);
          v.setInt16(POLL + 4, POLLIN, true);
          v.setInt16(POLL + 6, 0, true);
          n = 1;
        }
        for (const pipe of live) {
          if (n === 16) {
            break;
          }
          v.setInt32(POLL + 8 * n, pipe.fd, true);
          v.setInt16(POLL + 8 * n + 4, (pipe.collect ? POLLIN : 0) | (pipe.writing ? POLLOUT : 0), true);
          v.setInt16(POLL + 8 * n + 6, 0, true);
          pipe.slot = n++;
        }
        // (Without a descriptor for the child, look again every little while.)
        let waitMs = ended !== null ? 0 : childFd >= 0 ? -1 : 10;
        if (timeout && !killed) {
          const left = timeout - (hn.web.performance.now() - began);
          if (left <= 0) {
            kill(UV_ETIMEDOUT);
            continue;
          }
          waitMs = waitMs < 0 ? left : Math.min(waitMs, left);
        }
        let ts = 0;
        if (waitMs >= 0) {
          ts = S + SCRATCH - 8 * 16 - 16;
          v.setBigInt64(ts, BigInt(Math.floor(waitMs / 1000)), true);
          v.setBigInt64(ts + 8, BigInt(Math.floor((waitMs % 1000) * 1e6)), true);
        }
        const ready = sys5(SYS.ppoll, POLL, n, ts, 0, 0);
        if (ready <= 0) {
          continue;
        }
        for (const pipe of live) {
          const events = dv().getInt16(POLL + 8 * pipe.slot + 6, true);
          if (pipe.writing && (events & (POLLOUT | POLLERR | POLLHUP))) {
            if (pipe.input === null || pipe.sent >= pipe.input.length) {
              finishWriting(pipe);
            } else {
              const part = Math.min(pipe.input.length - pipe.sent, 32768);
              u8().set(pipe.input.subarray(pipe.sent, pipe.sent + part), S);
              const w = sys3(SYS.write, pipe.fd, S, part);
              if (w > 0) {
                pipe.sent += w;
                if (pipe.sent >= pipe.input.length) {
                  finishWriting(pipe);
                }
              } else if (w !== -EAGAIN && w !== -EINTR) {
                finishWriting(pipe);
              }
            }
          }
          if (pipe.open && pipe.collect && (events & (POLLIN | POLLERR | POLLHUP))) {
            const r = sys3(SYS.read, pipe.fd, S, 32768);
            if (r > 0) {
              pipe.chunks.push(hn.copy(S, r));
              pipe.size += r;
              if (pipe.size > maxBuffer) {
                kill(UV_ENOBUFS);
              }
            } else if (r !== -EAGAIN && r !== -EINTR) {
              sys1(SYS.close, pipe.fd);
              pipe.open = false;
            }
          }
        }
      }
      closeAll();
      if (childFd >= 0) {
        sys1(SYS.close, childFd);
      }
      if (ended === null) {
        const r = wait(pid, 0);
        ended = typeof r === "object" ? r : { code: 0, signal: 0 };
      }
      const output = [];
      for (let i = 0; i < stdio.length; i++) {
        output.push(null);
      }
      for (const pipe of pipes) {
        if (pipe.collect) {
          output[pipe.index] = Buffer.concat(pipe.chunks.map((c) => Buffer.from(c.buffer, c.byteOffset, c.byteLength)));
        }
      }
      const result = {
        status: ended.signal ? null : ended.code,
        signal: ended.signal ? signalNames[ended.signal] || null : null,
        output,
        pid,
      };
      if (error) {
        result.error = error;
      }
      return result;
    },
  }));
})
