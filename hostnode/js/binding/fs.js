// The fs binding (src/node_file.cc): files of the guest system, by its
// kernel's system calls.
//
// Every call is carried out at once, also those that lib/ makes with a
// request object or asks a promise for: node gives those to a pool of
// threads, and there are no threads here to give them to. Their answers are
// still delivered later, each on a turn of the loop of its own, in the order
// they were asked, so a program sees the same order of events; what it does
// not get is doing something else while a slow file is being read.
(function (hn) {
  "use strict";

  const { loop, data, SYS, sys1, sys2, sys3, sys4, sys5, sys6, dv, u8, S, SCRATCH, P0, P1 } = hn;
  const shapes = data.bindings;
  const C = shapes.constants.fs;
  const AT_FDCWD = -100, AT_SYMLINK_NOFOLLOW = 0x100, AT_REMOVEDIR = 0x200, AT_EMPTY_PATH = 0x1000;
  const O_CLOEXEC = 0o2000000, O_PATH = 0o10000000, O_DIRECTORY = 0o200000;
  const EINTR = 4, ENOENT = 2, EEXIST = 17, ENOTDIR = 20, EISDIR = 21, EINVAL = 22;
  const STATX_BASIC_BTIME = 0xfff;

  // What went wrong, on its way to becoming an exception or the error of a callback.
  class Fail {
    constructor(code, syscall, path, dest) {
      this.code = code;
      this.syscall = syscall;
      this.path = path;
      this.dest = dest;
    }
    error(top) {
      return hn.uvException(this.code, this.syscall, this.path, this.dest, top);
    }
  }
  const shown = (p) => (typeof p === "string" ? p : p === undefined ? undefined : hn.decoder.decode(p));
  const check = (r, syscall, path, dest) => {
    if (r < 0) {
      throw new Fail(r, syscall, shown(path), shown(dest));
    }
    return r;
  };
  const retry = (call) => {
    for (;;) {
      const r = call();
      if (r !== -EINTR) {
        return r;
      }
    }
  };

  // A larger place than the scratch block, for reading and writing a lot at a time.
  let big = 0;
  const BIG = 1 << 20;
  const room = (size) => {
    if (size <= SCRATCH) {
      return S;
    }
    if (!big) {
      big = hn.alloc(BIG);
    }
    return big;
  };
  const roomSize = (size) => (size <= SCRATCH ? SCRATCH : BIG);

  const position = (p) => (p === null || p === undefined ? -1 : typeof p === "bigint" ? Number(p) : p);
  const readInto = (fd, target, offset, length, pos) => {
    pos = position(pos);
    let done = 0;
    const place = room(length), size = roomSize(length);
    while (done < length) {
      const want = Math.min(length - done, size);
      const n = retry(() => (pos < 0 ? sys3(SYS.read, fd, place, want) : sys4(SYS.pread64, fd, place, want, pos + done)));
      if (n < 0) {
        if (done > 0) {
          break;
        }
        return n;
      }
      if (n > 0) {
        target.set(u8().subarray(place, place + n), offset + done);
      }
      done += n;
      if (n < want) {
        break;
      }
    }
    return done;
  };
  const writeFrom = (fd, source, offset, length, pos) => {
    pos = position(pos);
    let done = 0;
    const place = room(length), size = roomSize(length);
    while (done < length) {
      const want = Math.min(length - done, size);
      u8().set(source.subarray(offset + done, offset + done + want), place);
      const n = retry(() => (pos < 0 ? sys3(SYS.write, fd, place, want) : sys4(SYS.pwrite64, fd, place, want, pos + done)));
      if (n < 0) {
        if (done > 0) {
          break;
        }
        return n;
      }
      done += n;
      if (n < want) {
        break;
      }
    }
    return done;
  };
  const bytesOf = (v) => (v instanceof Uint8Array ? v : new Uint8Array(v.buffer, v.byteOffset, v.byteLength));

  // The whole of an open file.
  const readAll = (fd) => {
    const chunks = [];
    let total = 0;
    for (;;) {
      const n = retry(() => sys3(SYS.read, fd, room(BIG), BIG));
      if (n < 0) {
        return n;
      }
      if (n === 0) {
        break;
      }
      chunks.push(hn.copy(big, n));
      total += n;
    }
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

  // --- stat. The numbers go into arrays that lib/ reads them from (statValues), 18 for a file: see
  // lib/internal/fs/utils.js: getStatsFromBinding.
  const statValues = new Float64Array(36);
  const bigintStatValues = new BigInt64Array(36);
  const statFsValues = new Float64Array(7);
  const bigintStatFsValues = new BigInt64Array(7);
  const device = (major, minor) =>
    ((major & 0xfff) * 256) + (minor & 0xff) + ((minor >>> 8) * 4096 * 256) + (Math.floor(major / 4096) * 4294967296);
  const statx = (fd, pathAddress, flags) => retry(() => sys5(SYS.statx, fd, pathAddress, flags, STATX_BASIC_BTIME, S));
  const fillStat = (bigint, into) => {
    const v = dv();
    const fields = [
      device(v.getUint32(S + 136, true), v.getUint32(S + 140, true)),
      v.getUint16(S + 28, true),
      v.getUint32(S + 16, true),
      v.getUint32(S + 20, true),
      v.getUint32(S + 24, true),
      device(v.getUint32(S + 128, true), v.getUint32(S + 132, true)),
      v.getUint32(S + 4, true),
    ];
    const out = into || (bigint ? bigintStatValues : statValues);
    if (bigint) {
      for (let i = 0; i < 7; i++) {
        out[i] = BigInt(fields[i]);
      }
      out[7] = BigInt.asIntN(64, v.getBigUint64(S + 32, true));
      out[8] = BigInt.asIntN(64, v.getBigUint64(S + 40, true));
      out[9] = BigInt.asIntN(64, v.getBigUint64(S + 48, true));
      let at = 10;
      for (const time of [64, 112, 96, 80]) {	// access, modification, change, birth
        out[at++] = v.getBigInt64(S + time, true);
        out[at++] = BigInt(v.getUint32(S + time + 8, true));
      }
    } else {
      for (let i = 0; i < 7; i++) {
        out[i] = fields[i];
      }
      out[7] = Number(v.getBigUint64(S + 32, true));
      out[8] = Number(v.getBigUint64(S + 40, true));
      out[9] = Number(v.getBigUint64(S + 48, true));
      let at = 10;
      for (const time of [64, 112, 96, 80]) {
        out[at++] = Number(v.getBigInt64(S + time, true));
        out[at++] = v.getUint32(S + time + 8, true);
      }
    }
    return out;
  };
  const fresh = (array) => array.slice(0, 18);

  // Text handed back in the encoding lib/ asked for.
  const encoded = (raw, encoding) => {
    if (encoding === undefined || encoding === null || encoding === "utf8" || encoding === "utf-8") {
      return hn.decoder.decode(raw);
    }
    const { Buffer } = hn.requireBuiltin("buffer");
    const buffer = Buffer.from(raw.buffer, raw.byteOffset, raw.byteLength);
    return encoding === "buffer" ? buffer : buffer.toString(encoding);
  };
  const encodeString = (string, encoding) => {
    if (encoding === undefined || encoding === null || encoding === "utf8" || encoding === "utf-8") {
      return hn.encoder.encode(string);
    }
    const { Buffer } = hn.requireBuiltin("buffer");
    return Buffer.from(string, encoding);
  };

  const direntTypes = { 1: C.UV_DIRENT_FIFO, 2: C.UV_DIRENT_CHAR, 4: C.UV_DIRENT_DIR, 6: C.UV_DIRENT_BLOCK,
                        8: C.UV_DIRENT_FILE, 10: C.UV_DIRENT_LINK, 12: C.UV_DIRENT_SOCKET };
  // The entries of an open directory: names (as bytes) and kinds.
  const entries = (fd) => {
    const names = [], types = [];
    for (;;) {
      const n = retry(() => sys3(SYS.getdents64, fd, S, SCRATCH));
      if (n < 0) {
        return n;
      }
      if (n === 0) {
        break;
      }
      const m = u8(), v = dv();
      for (let at = S; at < S + n;) {
        const length = v.getUint16(at + 16, true);
        const type = m[at + 18];
        let end = at + 19;
        while (m[end]) {
          end++;
        }
        const isDot = m[at + 19] === 46 && (end === at + 20 || (m[at + 20] === 46 && end === at + 21));
        if (!isDot) {
          names.push(m.slice(at + 19, end));
          types.push(direntTypes[type] || C.UV_DIRENT_UNKNOWN);
        }
        at += length;
      }
    }
    return { names, types };
  };

  const open = (path, flags, mode) =>
    retry(() => sys4(SYS.openat, AT_FDCWD, hn.path(path, P0), flags | O_CLOEXEC | 0o100000, mode));	// O_LARGEFILE
  const close = (fd) => sys1(SYS.close, fd);

  const mkdirp = (path, mode) => {
    const p = shown(path);
    const r = sys3(SYS.mkdirat, AT_FDCWD, hn.path(p, P0), mode);
    if (r === 0) {
      return p;
    }
    if (r === -ENOENT) {
      const slash = p.replace(/\/+$/, "").lastIndexOf("/");
      if (slash <= 0) {
        throw new Fail(r, "mkdir", p);
      }
      const first = mkdirp(p.slice(0, slash), mode);
      const again = sys3(SYS.mkdirat, AT_FDCWD, hn.path(p, P0), mode);
      if (again < 0 && again !== -EEXIST) {
        throw new Fail(again, "mkdir", p);
      }
      return first === undefined ? p : first;
    }
    if (r === -EEXIST) {
      if (statx(AT_FDCWD, hn.path(p, P0), 0) === 0 && (dv().getUint16(S + 28, true) & 0o170000) === 0o040000) {
        return undefined;
      }
      throw new Fail(-EEXIST, "mkdir", p);
    }
    throw new Fail(r, "mkdir", p);
  };

  const timespecs = (atime, mtime) => {
    const v = dv();
    let at = S;
    for (const t of [atime, mtime]) {
      const seconds = Math.floor(t);
      v.setBigInt64(at, BigInt(seconds), true);
      v.setBigInt64(at + 8, BigInt(Math.round((t - seconds) * 1e9)), true);
      at += 16;
    }
    return S;
  };

  // --- The operations. Each takes its arguments without the request and returns the result or throws a Fail.
  const ops = {
    access(path, mode) {
      check(sys3(SYS.faccessat, AT_FDCWD, hn.path(path, P0), mode), "access", path);
    },
    close(fd) {
      check(close(fd), "close");
    },
    open(path, flags, mode) {
      return check(open(path, flags, mode), "open", path);
    },
    openFileHandle(path, flags, mode) {
      return new FileHandle(check(open(path, flags, mode), "open", path));
    },
    read(fd, buffer, offset, length, pos) {
      return check(readInto(fd, bytesOf(buffer), offset, length, pos), "read");
    },
    readBuffers(fd, buffers, pos) {
      pos = position(pos);
      let total = 0;
      for (const buffer of buffers) {
        const n = check(readInto(fd, bytesOf(buffer), 0, buffer.byteLength, pos < 0 ? -1 : pos + total), "read");
        total += n;
        if (n < buffer.byteLength) {
          break;
        }
      }
      return total;
    },
    fdatasync(fd) {
      check(sys1(SYS.fdatasync, fd), "fdatasync");
    },
    fsync(fd) {
      check(sys1(SYS.fsync, fd), "fsync");
    },
    rename(from, to) {
      check(sys5(SYS.renameat2, AT_FDCWD, hn.path(from, P0), AT_FDCWD, hn.path(to, P1), 0), "rename", from, to);
    },
    ftruncate(fd, length) {
      check(sys2(SYS.ftruncate, fd, typeof length === "bigint" ? Number(length) : length), "ftruncate");
    },
    rmdir(path) {
      check(sys3(SYS.unlinkat, AT_FDCWD, hn.path(path, P0), AT_REMOVEDIR), "rmdir", path);
    },
    mkdir(path, mode, recursive) {
      if (recursive) {
        return mkdirp(path, mode);
      }
      check(sys3(SYS.mkdirat, AT_FDCWD, hn.path(path, P0), mode), "mkdir", path);
      return undefined;
    },
    readdir(path, encoding, withFileTypes) {
      const fd = check(open(path, O_DIRECTORY, 0), "scandir", path);
      const list = entries(fd);
      close(fd);
      if (typeof list === "number") {
        throw new Fail(list, "scandir", shown(path));
      }
      const names = list.names.map((raw) => encoded(raw, encoding));
      return withFileTypes ? [names, list.types] : names;
    },
    stat(path, bigint) {
      check(statx(AT_FDCWD, hn.path(path, P0), 0), "stat", path);
      return fillStat(bigint);
    },
    lstat(path, bigint) {
      check(statx(AT_FDCWD, hn.path(path, P0), AT_SYMLINK_NOFOLLOW), "lstat", path);
      return fillStat(bigint);
    },
    fstat(fd, bigint) {
      check(statx(fd, hn.path("", P0), AT_EMPTY_PATH), "fstat");
      return fillStat(bigint);
    },
    statfs(path, bigint) {
      check(sys2(SYS.statfs, hn.path(path, P0), S), "statfs", path);
      const v = dv();
      const out = bigint ? bigintStatFsValues : statFsValues;
      for (let i = 0; i < 7; i++) {
        const value = v.getBigInt64(S + 8 * i, true);
        out[i] = bigint ? value : Number(value);
      }
      return out;
    },
    link(existing, path) {
      check(sys5(SYS.linkat, AT_FDCWD, hn.path(existing, P0), AT_FDCWD, hn.path(path, P1), 0), "link", existing, path);
    },
    symlink(target, path) {
      check(sys3(SYS.symlinkat, hn.path(target, P0), AT_FDCWD, hn.path(path, P1)), "symlink", target, path);
    },
    readlink(path, encoding) {
      const n = check(sys4(SYS.readlinkat, AT_FDCWD, hn.path(path, P0), S, SCRATCH), "readlink", path);
      return encoded(hn.copy(S, n), encoding);
    },
    unlink(path) {
      check(sys3(SYS.unlinkat, AT_FDCWD, hn.path(path, P0), 0), "unlink", path);
    },
    writeBuffer(fd, buffer, offset, length, pos) {
      return check(writeFrom(fd, bytesOf(buffer), offset, length, pos), "write");
    },
    writeBuffers(fd, buffers, pos) {
      pos = position(pos);
      let total = 0;
      for (const buffer of buffers) {
        const n = check(writeFrom(fd, bytesOf(buffer), 0, buffer.byteLength, pos < 0 ? -1 : pos + total), "write");
        total += n;
        if (n < buffer.byteLength) {
          break;
        }
      }
      return total;
    },
    writeString(fd, string, pos, encoding) {
      const raw = encodeString(string, encoding);
      return check(writeFrom(fd, raw, 0, raw.length, pos), "write");
    },
    realpath(path, encoding) {
      // The kernel resolves the path when it opens it, and says under /proc what it came to.
      const fd = check(open(path, O_PATH, 0), "realpath", path);
      const name = "/proc/self/fd/" + fd;
      const n = sys4(SYS.readlinkat, AT_FDCWD, hn.path(name, P0), S, SCRATCH);
      close(fd);
      check(n, "realpath", path);
      return encoded(hn.copy(S, n), encoding);
    },
    copyFile(from, to, mode) {
      const source = check(open(from, 0, 0), "copyfile", from, to);
      let target = -1;
      try {
        check(statx(source, hn.path("", P0), AT_EMPTY_PATH), "copyfile", from, to);
        const permissions = dv().getUint16(S + 28, true) & 0o7777;
        const flags = C.O_WRONLY | C.O_CREAT | ((mode & C.COPYFILE_EXCL) ? C.O_EXCL : C.O_TRUNC);
        target = check(retry(() => sys4(SYS.openat, AT_FDCWD, hn.path(to, P0), flags | O_CLOEXEC, permissions)),
                       "copyfile", from, to);
        for (;;) {
          const n = check(retry(() => sys3(SYS.read, source, room(BIG), BIG)), "copyfile", from, to);
          if (n === 0) {
            break;
          }
          let done = 0;
          while (done < n) {
            done += check(retry(() => sys3(SYS.write, target, big + done, n - done)), "copyfile", from, to);
          }
        }
        sys2(SYS.fchmod, target, permissions);
      } finally {
        close(source);
        if (target >= 0) {
          close(target);
        }
      }
    },
    chmod(path, mode) {
      check(sys3(SYS.fchmodat, AT_FDCWD, hn.path(path, P0), mode), "chmod", path);
    },
    fchmod(fd, mode) {
      check(sys2(SYS.fchmod, fd, mode), "fchmod");
    },
    chown(path, uid, gid) {
      check(sys5(SYS.fchownat, AT_FDCWD, hn.path(path, P0), uid, gid, 0), "chown", path);
    },
    fchown(fd, uid, gid) {
      check(sys3(SYS.fchown, fd, uid, gid), "fchown");
    },
    lchown(path, uid, gid) {
      check(sys5(SYS.fchownat, AT_FDCWD, hn.path(path, P0), uid, gid, AT_SYMLINK_NOFOLLOW), "lchown", path);
    },
    utimes(path, atime, mtime) {
      check(sys4(SYS.utimensat, AT_FDCWD, hn.path(path, P0), timespecs(atime, mtime), 0), "utime", path);
    },
    futimes(fd, atime, mtime) {
      check(sys4(SYS.utimensat, fd, 0, timespecs(atime, mtime), 0), "futime");
    },
    lutimes(path, atime, mtime) {
      check(sys4(SYS.utimensat, AT_FDCWD, hn.path(path, P0), timespecs(atime, mtime), AT_SYMLINK_NOFOLLOW), "lutime", path);
    },
    mkdtemp(prefix, encoding) {
      const letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
      const base = shown(prefix);
      for (let attempt = 0; attempt < 100; attempt++) {
        let name = base;
        const random = new Uint8Array(6);
        hn.web.crypto.getRandomValues(random);
        for (const r of random) {
          name += letters[r % letters.length];
        }
        const r = sys3(SYS.mkdirat, AT_FDCWD, hn.path(name, P0), 0o700);
        if (r === 0) {
          return encoded(hn.encoder.encode(name), encoding);
        }
        if (r !== -EEXIST) {
          throw new Fail(r, "mkdtemp", base + "XXXXXX");
        }
      }
      throw new Fail(-EEXIST, "mkdtemp", base + "XXXXXX");
    },
  };

  // --- Requests (lib/fs.js makes an FSReqCallback and sets oncomplete on it).
  class FSReqCallback extends hn.AsyncWrap {
    constructor(bigint) {
      super("FSREQCALLBACK");
    }
  }
  // An open file as lib/internal/fs/promises.js holds it.
  class FileHandle extends hn.AsyncWrap {
    constructor(fd) {
      super("FILEHANDLE");
      this.__fd = fd;
    }
    get fd() {
      return this.__fd;
    }
    getAsyncId() {
      return super.getAsyncId();
    }
    close() {
      const fd = this.__fd;
      this.__fd = -1;
      return later(() => {
        if (fd >= 0) {
          check(close(fd), "close");
        }
      });
    }
    releaseFD() {
      const fd = this.__fd;
      this.__fd = -1;
      return fd;
    }
  }
  Object.defineProperty(FileHandle.prototype, "__fd", { writable: true, enumerable: false, value: -1 });

  // Do it now; hand the outcome to a promise on a later turn.
  const later = (run) => {
    let value, failure = null;
    try {
      value = run();
    } catch (error) {
      if (!(error instanceof Fail)) {
        throw error;
      }
      failure = error.error();
    }
    loop.refs++;
    return new Promise((resolve, reject) => {
      loop.enqueue(() => {
        loop.refs--;
        if (failure) {
          reject(failure);
        } else {
          resolve(value);
        }
      });
    });
  };

  const kUsePromises = hn.internalBinding("symbols").fs_use_promises_symbol;
  // The place of the request among the arguments of each call (after the last argument proper).
  const requestAt = {
    access: 2, close: 1, open: 3, openFileHandle: 3, read: 5, readBuffers: 3, fdatasync: 1, fsync: 1, rename: 2,
    ftruncate: 2, rmdir: 1, mkdir: 3, readdir: 3, stat: 2, lstat: 2, fstat: 2, statfs: 2, link: 2, symlink: 3,
    readlink: 2, unlink: 1, writeBuffer: 5, writeBuffers: 3, writeString: 4, realpath: 2, copyFile: 3, chmod: 2,
    fchmod: 2, chown: 3, fchown: 3, lchown: 3, utimes: 3, futimes: 3, lutimes: 3, mkdtemp: 2,
  };
  const isStat = { stat: true, lstat: true, fstat: true };

  hn.binding("fs", () => {
    const binding = {
      kFsStatsFieldsNumber: 18,
      statValues, bigintStatValues, statFsValues, bigintStatFsValues,
      kUsePromises,
      FSReqCallback,
      FileHandle,
    };
    for (const name of Object.keys(ops)) {
      const op = ops[name];
      const at = requestAt[name];
      binding[name] = function (...args) {
        const request = args[at];
        if (request === kUsePromises) {
          return later(() => {
            const value = op(...args.slice(0, at));
            // A promise gets numbers of its own: the shared array is overwritten by the next stat.
            return isStat[name] ? fresh(value) : name === "statfs" ? value.slice() : value;
          });
        }
        if (request !== undefined && request !== null && typeof request === "object") {
          let value, failure = null;
          try {
            value = op(...args.slice(0, at));
            if (isStat[name]) {
              value = fresh(value);
            } else if (name === "statfs") {
              value = value.slice();
            }
          } catch (error) {
            if (!(error instanceof Fail)) {
              throw error;
            }
            failure = error.error();
          }
          loop.refs++;
          loop.enqueue(() => {
            loop.refs--;
            hn.makeCallback(request, () => {
              if (failure) {
                request.oncomplete(failure);
              } else if (value === undefined) {
                request.oncomplete(null);
              } else {
                request.oncomplete(null, value);
              }
            });
          });
          return undefined;
        }
        try {
          return op(...args.slice(0, at));
        } catch (error) {
          if (error instanceof Fail) {
            // (lib/ asks stat for nothing instead of an exception when a file may well not be there.)
            if ((name === "stat" || name === "lstat") && args[3] === false &&
                (error.code === -ENOENT || error.code === -ENOTDIR)) {
              return undefined;
            }
            if (name === "fstat" && args[3] === true) {
              return undefined;
            }
            throw error.error(binding[name]);
          }
          throw error;
        }
      };
    }

    // Calls without a request.
    binding.existsSync = (path) => sys3(SYS.faccessat, AT_FDCWD, hn.path(path, P0), 0) === 0;
    binding.readFileUtf8 = (pathOrFd, flags) => {
      const isFd = typeof pathOrFd === "number";
      const fd = isFd ? pathOrFd : open(pathOrFd, flags, 0o666);
      if (fd < 0) {
        throw hn.uvException(fd, "open", shown(pathOrFd));
      }
      const all = readAll(fd);
      if (!isFd) {
        close(fd);
      }
      if (typeof all === "number") {
        throw hn.uvException(all, "read", isFd ? undefined : shown(pathOrFd));
      }
      return hn.decoder.decode(all);
    };
    binding.writeFileUtf8 = (pathOrFd, text, flags, mode) => {
      const isFd = typeof pathOrFd === "number";
      const fd = isFd ? pathOrFd : open(pathOrFd, flags, mode);
      if (fd < 0) {
        throw hn.uvException(fd, "open", shown(pathOrFd));
      }
      const raw = hn.encoder.encode(text);
      let done = 0;
      while (done < raw.length) {
        const n = writeFrom(fd, raw, done, raw.length - done, -1);
        if (n < 0) {
          if (!isFd) {
            close(fd);
          }
          throw hn.uvException(n, "write", isFd ? undefined : shown(pathOrFd));
        }
        done += n;
      }
      if (!isFd) {
        close(fd);
      }
    };
    // 0: a file, 1: a directory, negative: neither (lib/internal/modules/cjs/loader.js: stat).
    binding.internalModuleStat = (path) => {
      const r = statx(AT_FDCWD, hn.path(path, P0), 0);
      if (r < 0) {
        return r;
      }
      return (dv().getUint16(S + 28, true) & 0o170000) === 0o040000 ? 1 : 0;
    };
    // The text of a package.json, and whether it is worth parsing.
    binding.internalModuleReadJSON = (path) => {
      const fd = open(path, 0, 0);
      if (fd < 0) {
        return [];
      }
      const all = readAll(fd);
      close(fd);
      if (typeof all === "number") {
        return [];
      }
      const text = hn.decoder.decode(all[0] === 0xef && all[1] === 0xbb && all[2] === 0xbf ? all.subarray(3) : all);
      return [text, /"(name|main|exports|imports|type)"/.test(text)];
    };
    binding.getFormatOfExtensionlessFile = (path) => {
      const fd = open(path, 0, 0);
      if (fd < 0) {
        return C.EXTENSIONLESS_FORMAT_JAVASCRIPT;
      }
      const n = sys3(SYS.read, fd, S, 4);
      close(fd);
      const m = u8();
      return n === 4 && m[S] === 0 && m[S + 1] === 0x61 && m[S + 2] === 0x73 && m[S + 3] === 0x6d ?
        C.EXTENSIONLESS_FORMAT_WASM : C.EXTENSIONLESS_FORMAT_JAVASCRIPT;
    };
    // lib/internal/modules/esm/resolve.js: legacyMainResolve. Which of the usual names a package's entry has.
    binding.legacyMainResolve = (packageJSONUrl, main, base) => {
      const { fileURLToPath } = hn.requireBuiltin("internal/url");
      const path = hn.requireBuiltin("path");
      const directory = path.dirname(fileURLToPath(packageJSONUrl));
      const isFile = (p) => binding.internalModuleStat(p) === 0;
      const endings = ["", ".js", ".json", ".node", "/index.js", "/index.json", "/index.node"];
      if (main !== undefined) {
        for (let i = 0; i < endings.length; i++) {
          if (isFile(path.resolve(directory, main + endings[i]))) {
            return i;
          }
        }
      }
      const index = ["./index.js", "./index.json", "./index.node"];
      for (let i = 0; i < index.length; i++) {
        if (isFile(path.resolve(directory, index[i]))) {
          return 7 + i;
        }
      }
      const { codes } = hn.requireBuiltin("internal/errors");
      throw new codes.ERR_MODULE_NOT_FOUND(directory, base === undefined ? undefined : fileURLToPath(base), "package");
    };
    // fs.watchFile (libuv's uv_fs_poll): the file is looked at every so often, by a timer of the kernel's
    // (timerfd), which the loop waits for like a descriptor; a timer of the browser's would not fire while
    // the loop sleeps in the kernel.
    const TFD_FLAGS = 0o4000 | 0o2000000;	// non-blocking, close on exec
    const CLOCK_MONOTONIC = 1;
    binding.StatWatcher = class StatWatcher extends hn.HandleWrap {
      constructor(bigint) {
        super("STATWATCHER");
        this.__bigint = !!bigint;
        this.__fd = -1;
        this.__watcher = null;
        this.__previous = null;
        this.__status = 0;
      }
      __look() {
        const status = statx(AT_FDCWD, hn.path(this.__path, P0), 0);
        const now = new Float64Array(18);	// (libuv: a file that is not there has zeroes for its numbers)
        if (status >= 0) {
          fillStat(false, now);
        }
        return { status: status < 0 ? status : 0, stats: now };
      }
      start(path, interval) {
        this.__path = String(path);
        const first = this.__look();
        this.__status = first.status;
        this.__previous = first.stats;
        const fd = sys2(SYS.timerfd_create, CLOCK_MONOTONIC, TFD_FLAGS);
        if (fd < 0) {
          return fd;
        }
        const ms = Math.max(1, interval | 0);
        const v = dv();
        for (const at of [S, S + 16]) {	// the interval, then the first expiry
          v.setBigInt64(at, BigInt(Math.floor(ms / 1000)), true);
          v.setBigInt64(at + 8, BigInt((ms % 1000) * 1e6), true);
        }
        const r = sys4(SYS.timerfd_settime, fd, 0, S, 0);
        if (r < 0) {
          sys1(SYS.close, fd);
          return r;
        }
        this.__fd = fd;
        this.__watcher = loop.watch(fd, 1, () => {
          sys3(SYS.read, fd, S, 8);	// (how many times it went off: not needed)
          const { status, stats } = this.__look();
          let same = status === this.__status;
          for (let i = 0; same && i < 18; i++) {
            same = stats[i] === this.__previous[i];
          }
          if (same) {
            return;
          }
          const both = this.__bigint ? new BigInt64Array(36) : new Float64Array(36);
          for (let i = 0; i < 18; i++) {
            both[i] = this.__bigint ? BigInt(stats[i]) : stats[i];
            both[18 + i] = this.__bigint ? BigInt(this.__previous[i]) : this.__previous[i];
          }
          this.__status = status;
          this.__previous = stats;
          hn.makeCallback(this, () => this.onchange(status, both));
        });
        this.__setActive(true);
        return 0;
      }
      __close() {
        if (this.__watcher) {
          this.__watcher.close();
          this.__watcher = null;
          sys1(SYS.close, this.__fd);
          this.__fd = -1;
        }
      }
    };
    for (const name of ["__bigint", "__fd", "__watcher", "__previous", "__status", "__path"]) {
      Object.defineProperty(binding.StatWatcher.prototype, name, { writable: true, enumerable: false, value: null });
    }
    return binding;
  });

  // --- fs_dir (src/node_dir.cc): a directory read a few entries at a time.
  hn.binding("fs_dir", () => {
    class DirHandle extends hn.AsyncWrap {
      constructor(fd) {
        super("DIRHANDLE");
        this.__fd = fd;
        this.__list = null;
        this.__at = 0;
      }
      __read(encoding, count) {
        if (this.__list === null) {
          const list = entries(this.__fd);
          if (typeof list === "number") {
            throw new Fail(list, "scandir");
          }
          this.__list = list;
        }
        if (this.__at >= this.__list.names.length) {
          return null;
        }
        const out = [];
        for (let i = 0; i < count && this.__at < this.__list.names.length; i++, this.__at++) {
          out.push(encoded(this.__list.names[this.__at], encoding), this.__list.types[this.__at]);
        }
        return out;
      }
      read(encoding, count, request) {
        return dispatch(request, () => this.__read(encoding, count));
      }
      close(request) {
        return dispatch(request, () => {
          const fd = this.__fd;
          this.__fd = -1;
          if (fd >= 0) {
            check(close(fd), "closedir");
          }
        });
      }
    }
    const dispatch = (request, run) => {
      if (request === undefined) {
        try {
          return run();
        } catch (error) {
          throw error instanceof Fail ? error.error() : error;
        }
      }
      let value, failure = null;
      try {
        value = run();
      } catch (error) {
        if (!(error instanceof Fail)) {
          throw error;
        }
        failure = error.error();
      }
      loop.refs++;
      loop.enqueue(() => {
        loop.refs--;
        hn.makeCallback(request, () => (failure ? request.oncomplete(failure) : request.oncomplete(null, value)));
      });
      return undefined;
    };
    const opendir = (path) => new DirHandle(check(open(path, O_DIRECTORY, 0), "opendir", path));
    return {
      DirHandle,
      opendir(path, encoding, request) {
        return dispatch(request, () => opendir(path));
      },
      opendirSync(path) {
        return dispatch(undefined, () => opendir(path));
      },
    };
  });

  // --- fs_event_wrap (src/fs_event_wrap.cc, libuv's uv_fs_event on inotify): fs.watch. One inotify
  // descriptor for all watchers; the kernel gives the same watch number for the same file however often it
  // is added, so the watchers of one are kept together and the watch removed with the last of them.
  hn.binding("fs_event_wrap", () => {
    const IN_MODIFY = 0x2, IN_ATTRIB = 0x4, IN_MOVED_FROM = 0x40, IN_MOVED_TO = 0x80, IN_CREATE = 0x100,
      IN_DELETE = 0x200, IN_DELETE_SELF = 0x400, IN_MOVE_SELF = 0x800, IN_IGNORED = 0x8000, IN_Q_OVERFLOW = 0x4000;
    const IN_NONBLOCK = 0o4000, IN_CLOEXEC = 0o2000000;
    const WANTED = IN_ATTRIB | IN_CREATE | IN_MODIFY | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO;
    let fd = -1;
    const byWatch = new Map();	// watch number -> Set of FSEvent
    const read = () => {
      for (;;) {
        const n = sys3(SYS.read, fd, S, SCRATCH);
        if (n <= 0) {
          return;
        }
        const v = dv();
        for (let at = S; at < S + n;) {
          const wd = v.getInt32(at, true);
          const mask = v.getUint32(at + 4, true);
          const length = v.getUint32(at + 12, true);
          let name = null;
          if (length) {
            let end = at + 16;
            while (end < at + 16 + length && u8()[end]) {
              end++;
            }
            name = hn.copy(at + 16, end - (at + 16));
          }
          at += 16 + length;
          const set = byWatch.get(wd);
          if (!set) {
            continue;
          }
          if (mask & IN_Q_OVERFLOW) {
            continue;
          }
          if (mask & IN_IGNORED) {
            // The watch is gone (the file was deleted, or unmounted): the kernel has removed it already.
            byWatch.delete(wd);
            for (const watcher of set) {
              watcher.__wd = -1;
            }
            continue;
          }
          const type = mask & (IN_ATTRIB | IN_MODIFY) ? "change" : "rename";
          for (const watcher of [...set]) {
            if (!watcher.__closed) {
              watcher.__event(type, name);
            }
          }
        }
      }
    };
    class FSEvent extends hn.HandleWrap {
      constructor() {
        super("FSEVENTWRAP");
        this.initialized = false;
        this.__wd = -1;
        this.__encoding = "utf8";
        this.__basename = null;
      }
      start(path, persistent, recursive, encoding) {
        if (fd < 0) {
          fd = sys1(SYS.inotify_init1, IN_NONBLOCK | IN_CLOEXEC);
          if (fd < 0) {
            return fd;
          }
          loop.watch(fd, 1, read);
        }
        const wd = sys3(SYS.inotify_add_watch, fd, hn.path(path, P0), WANTED);
        if (wd < 0) {
          return wd;
        }
        this.__wd = wd;
        this.__encoding = encoding || "utf8";
        // (libuv: an event about the watched file itself carries the file's own name.)
        const text = String(path).replace(/\/+$/, "");
        this.__basename = hn.encoder.encode(text.slice(text.lastIndexOf("/") + 1));
        let set = byWatch.get(wd);
        if (!set) {
          byWatch.set(wd, (set = new Set()));
        }
        set.add(this);
        this.initialized = true;
        this.__setActive(true);
        if (!persistent) {
          this.unref();
        }
        return 0;
      }
      __event(type, name) {
        const bytes = name || this.__basename;
        let filename;
        const { Buffer } = hn.requireBuiltin("buffer");
        if (this.__encoding === "buffer") {
          filename = Buffer.from(bytes);
        } else {
          filename = Buffer.from(bytes).toString(this.__encoding);
        }
        hn.makeCallback(this, () => this.onchange(0, type, filename));
      }
      __close() {
        if (this.__wd >= 0) {
          const set = byWatch.get(this.__wd);
          if (set) {
            set.delete(this);
            if (set.size === 0) {
              byWatch.delete(this.__wd);
              sys2(SYS.inotify_rm_watch, fd, this.__wd);
            }
          }
          this.__wd = -1;
        }
        this.initialized = false;
      }
    }
    for (const name of ["__wd", "__encoding", "__basename"]) {
      Object.defineProperty(FSEvent.prototype, name, { writable: true, enumerable: false, value: null });
    }
    return { FSEvent };
  });
})
