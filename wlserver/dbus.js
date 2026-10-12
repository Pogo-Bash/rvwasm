// D-Bus, the client's side, on the session bus: enough of it for the server
// to be the KDE services that System Settings and Plasma's tools talk to
// (kwin.js: org.kde.KWin and its virtual desktops, the shell's wallpaper;
// notify.js: notifications and tray icons). Written from the D-Bus
// Specification (version 0.41: "Message Protocol", "Authentication
// Protocol", "Message Bus Specification").
//
// The wire, in short (we always write little-endian, 'l'):
//   a message is a fixed header (endianness, type, flags, version 1, the
//   body's length, a serial), an array of header fields a(yv) (path,
//   interface, member, error name, reply serial, destination, sender,
//   signature), padding to 8, and the body, whose signature says its types;
//   every value is aligned to its own size from the start of the message
//   (strings and arrays to 4 for their length, structs and dict entries to 8,
//   variants carry their signature).
//   Before the first message: a NUL byte and the authentication exchange in
//   text lines (AUTH EXTERNAL with our uid in hex, then BEGIN). Then Hello
//   gives us our unique name, and RequestName the well-known ones.
//
// The bus is $DBUS_SESSION_BUS_ADDRESS (`desktop wayland` starts a session
// bus at $XDG_RUNTIME_DIR/bus and gives it to the server and every program).
// Without one, nothing here runs and the services are simply not there.
//
// Use:
//   ws.dbus.export(path, interface, { methods: { Name: { in: "us", out: "b", fn(args, message) } },
//                                     properties: { name: { type: "u", get(), set(value) } },
//                                     signals: { name: "s" } })
//   ws.dbus.emit(path, interface, member, signature, [args])
//   ws.dbus.own(name)                         the well-known name
//   ws.dbus.call(destination, path, interface, member, signature, [args]) -> Promise of the reply's values
//   ws.dbus.match(rule, handler(message))     a signal rule (AddMatch) and what to do with matching signals
//   ws.dbus.V(signature, value)               a variant
// A method's fn returns the out values (an array, or one value for a single out argument), or throws
// ws.dbus.Fail(name, text) for an error reply. Values: numbers (BigInt or Number for x and t), strings,
// booleans, arrays, Uint8Array for ay, arrays of [key, value] pairs or plain objects for dicts, arrays for
// structs, V for variants (a plain value in a variant is given a type: string s, boolean b, integer i,
// other numbers d, Uint8Array ay, array av... V says better). Variants read back as their values.
(function (ws) {
  "use strict";

  const { sys1, sys3, sys5, SYS, u8, dv, log, envp, alloc, path, P0 } = ws;
  const AF_UNIX = 1, SOCK_STREAM = 1, SOCK_CLOEXEC = 0o2000000;
  const F_SETFL = 4, O_NONBLOCK = 0o4000;
  const EINTR = 4, EAGAIN = 11;
  const N_GETUID = 174;
  const POLLIN = 1;
  const MAX_MESSAGE = 128 << 20;	// (the spec's maximum)

  const T_CALL = 1, T_RETURN = 2, T_ERROR = 3, T_SIGNAL = 4;
  const NO_REPLY = 1;
  const F_PATH = 1, F_INTERFACE = 2, F_MEMBER = 3, F_ERROR = 4, F_REPLY_SERIAL = 5, F_DESTINATION = 6,
        F_SENDER = 7, F_SIGNATURE = 8;

  const env = (name) => {
    const e = envp.find((x) => x.startsWith(name + "="));
    return e ? e.slice(name.length + 1) : null;
  };
  const debug = env("WLSERVER_DBUS_DEBUG") === "1";

  const encoder = new TextEncoder();
  const decoder = new TextDecoder("utf-8");

  class V {
    constructor(sig, value) {
      this.sig = sig;
      this.value = value;
    }
  }
  class Fail extends Error {
    constructor(name, text) {
      super(text || name);
      this.dbusName = name;
    }
  }

  // ---- signatures

  // Where the complete type that begins at sig[i] ends.
  const typeEnd = (sig, i) => {
    const c = sig[i];
    if (c === "a") {
      return typeEnd(sig, i + 1);
    }
    if (c === "(" || c === "{") {
      const close = c === "(" ? ")" : "}";
      let j = i + 1;
      while (sig[j] !== close) {
        if (j >= sig.length) {
          throw new Error("dbus: bad signature " + sig);
        }
        j = typeEnd(sig, j);
      }
      return j + 1;
    }
    if (!"ybnqiuxtdsogvh".includes(c)) {
      throw new Error("dbus: bad signature " + sig);
    }
    return i + 1;
  };
  const types = (sig) => {
    const list = [];
    for (let i = 0; i < sig.length;) {
      const j = typeEnd(sig, i);
      list.push(sig.slice(i, j));
      i = j;
    }
    return list;
  };
  const alignOf = (t) => ({ y: 1, g: 1, v: 1, n: 2, q: 2, b: 4, i: 4, u: 4, h: 4, s: 4, o: 4, a: 4, x: 8, t: 8, d: 8, "(": 8, "{": 8 })[t[0]];

  // The type of a plain value put in a variant.
  const guess = (value) => {
    if (value instanceof V) {
      return value;
    }
    if (typeof value === "string") {
      return new V("s", value);
    }
    if (typeof value === "boolean") {
      return new V("b", value);
    }
    if (typeof value === "bigint") {
      return new V("x", value);
    }
    if (typeof value === "number") {
      return new V(Number.isInteger(value) && value >= -0x80000000 && value <= 0x7fffffff ? "i" : "d", value);
    }
    if (value instanceof Uint8Array) {
      return new V("ay", value);
    }
    if (Array.isArray(value)) {
      return new V("av", value);
    }
    if (value && typeof value === "object") {
      return new V("a{sv}", value);
    }
    return new V("s", String(value));
  };

  // ---- writing

  class Writer {
    constructor() {
      this.bytes = new Uint8Array(512);
      this.view = new DataView(this.bytes.buffer);
      this.length = 0;
    }
    room(n) {
      if (this.length + n > this.bytes.length) {
        let size = this.bytes.length * 2;
        while (this.length + n > size) {
          size *= 2;
        }
        const bigger = new Uint8Array(size);
        bigger.set(this.bytes.subarray(0, this.length));
        this.bytes = bigger;
        this.view = new DataView(bigger.buffer);
      }
    }
    align(n) {
      const pad = (n - (this.length % n)) % n;
      this.room(pad);
      this.bytes.fill(0, this.length, this.length + pad);
      this.length += pad;
    }
    put(type, value) {
      const c = type[0];
      this.align(alignOf(type));
      const v = this.view;
      switch (c) {
      case "y":
        this.room(1);
        v.setUint8(this.length, Number(value) & 0xff);
        this.length += 1;
        return;
      case "b":
        this.room(4);
        v.setUint32(this.length, value ? 1 : 0, true);
        this.length += 4;
        return;
      case "n":
      case "q":
        this.room(2);
        v.setUint16(this.length, Number(value) & 0xffff, true);
        this.length += 2;
        return;
      case "i":
      case "u":
      case "h":
        this.room(4);
        v.setUint32(this.length, Number(value) >>> 0, true);
        this.length += 4;
        return;
      case "x":
      case "t":
        this.room(8);
        v.setBigUint64(this.length, BigInt.asUintN(64, BigInt(typeof value === "number" ? Math.trunc(value) : value)), true);
        this.length += 8;
        return;
      case "d":
        this.room(8);
        v.setFloat64(this.length, Number(value), true);
        this.length += 8;
        return;
      case "s":
      case "o": {
        const raw = encoder.encode(String(value));
        this.room(5 + raw.length);
        this.view.setUint32(this.length, raw.length, true);
        this.bytes.set(raw, this.length + 4);
        this.bytes[this.length + 4 + raw.length] = 0;
        this.length += 5 + raw.length;
        return;
      }
      case "g": {
        const raw = encoder.encode(String(value));
        this.room(2 + raw.length);
        this.bytes[this.length] = raw.length;
        this.bytes.set(raw, this.length + 1);
        this.bytes[this.length + 1 + raw.length] = 0;
        this.length += 2 + raw.length;
        return;
      }
      case "v": {
        const g = guess(value);
        this.put("g", g.sig);
        this.put(g.sig, g.value);
        return;
      }
      case "(": {
        const inner = types(type.slice(1, -1));
        inner.forEach((t, i) => this.put(t, value[i]));
        return;
      }
      case "a": {
        const elem = type.slice(1);
        this.room(4);
        const at = this.length;
        this.length += 4;
        this.align(alignOf(elem));
        const start = this.length;
        if (elem === "y" && value instanceof Uint8Array) {
          this.room(value.length);
          this.bytes.set(value, this.length);
          this.length += value.length;
        } else if (elem[0] === "{") {
          const [kt, vt] = types(elem.slice(1, -1));
          const entries = value instanceof Map ? [...value.entries()] : Array.isArray(value) ? value : Object.entries(value || {});
          for (const [k, val] of entries) {
            this.align(8);
            this.put(kt, k);
            this.put(vt, val);
          }
        } else {
          for (const item of value || []) {
            this.put(elem, item);
          }
        }
        this.view.setUint32(at, this.length - start, true);
        return;
      }
      default:
        throw new Error("dbus: cannot write type " + type);
      }
    }
  }

  // ---- reading

  class Reader {
    constructor(bytes, little) {
      this.bytes = bytes;
      this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      this.little = little;
      this.at = 0;
    }
    align(n) {
      this.at += (n - (this.at % n)) % n;
    }
    get(type, depth = 0) {
      if (depth > 64) {
        throw new Error("dbus: nested too deep");
      }
      const c = type[0];
      this.align(alignOf(type));
      const v = this.view, L = this.little;
      let r;
      switch (c) {
      case "y":
        r = v.getUint8(this.at);
        this.at += 1;
        return r;
      case "b":
        r = v.getUint32(this.at, L) !== 0;
        this.at += 4;
        return r;
      case "n":
        r = v.getInt16(this.at, L);
        this.at += 2;
        return r;
      case "q":
        r = v.getUint16(this.at, L);
        this.at += 2;
        return r;
      case "i":
        r = v.getInt32(this.at, L);
        this.at += 4;
        return r;
      case "u":
      case "h":
        r = v.getUint32(this.at, L);
        this.at += 4;
        return r;
      case "x":
        r = Number(v.getBigInt64(this.at, L));
        this.at += 8;
        return r;
      case "t":
        r = Number(v.getBigUint64(this.at, L));
        this.at += 8;
        return r;
      case "d":
        r = v.getFloat64(this.at, L);
        this.at += 8;
        return r;
      case "s":
      case "o": {
        const n = v.getUint32(this.at, L);
        r = decoder.decode(this.bytes.subarray(this.at + 4, this.at + 4 + n));
        this.at += 5 + n;
        return r;
      }
      case "g": {
        const n = v.getUint8(this.at);
        r = decoder.decode(this.bytes.subarray(this.at + 1, this.at + 1 + n));
        this.at += 2 + n;
        return r;
      }
      case "v": {
        const sig = this.get("g");
        if (types(sig).length !== 1) {
          throw new Error("dbus: a variant of " + sig);
        }
        return this.get(sig, depth + 1);
      }
      case "(":
        return types(type.slice(1, -1)).map((t) => this.get(t, depth + 1));
      case "a": {
        const n = v.getUint32(this.at, L);
        this.at += 4;
        const elem = type.slice(1);
        this.align(alignOf(elem));
        const end = this.at + n;
        if (n > this.bytes.length - this.at + 8) {
          throw new Error("dbus: an array longer than the message");
        }
        if (elem === "y") {
          r = this.bytes.slice(this.at, end);
          this.at = end;
          return r;
        }
        if (elem[0] === "{") {
          const [kt, vt] = types(elem.slice(1, -1));
          r = {};
          while (this.at < end) {
            this.align(8);
            const k = this.get(kt, depth + 1);
            r[k] = this.get(vt, depth + 1);
          }
          return r;
        }
        r = [];
        while (this.at < end) {
          r.push(this.get(elem, depth + 1));
        }
        return r;
      }
      default:
        throw new Error("dbus: cannot read type " + type);
      }
    }
  }

  // ---- messages

  let serial = 0;
  const message = (type, fields, sig, values, flags = 0) => {
    const body = new Writer();
    if (sig) {
      types(sig).forEach((t, i) => body.put(t, values[i]));
    }
    const w = new Writer();
    const s = ++serial;
    w.put("y", 0x6c);	// 'l'
    w.put("y", type);
    w.put("y", flags);
    w.put("y", 1);
    w.put("u", body.length);
    w.put("u", s);
    const list = [];
    const add = (code, t, value) => {
      if (value !== undefined && value !== null) {
        list.push([code, new V(t, value)]);
      }
    };
    add(F_PATH, "o", fields.path);
    add(F_INTERFACE, "s", fields.interface);
    add(F_MEMBER, "s", fields.member);
    add(F_ERROR, "s", fields.error);
    add(F_REPLY_SERIAL, "u", fields.replySerial);
    add(F_DESTINATION, "s", fields.destination);
    if (sig) {
      add(F_SIGNATURE, "g", sig);
    }
    w.put("a(yv)", list);
    w.align(8);
    w.room(body.length);
    w.bytes.set(body.bytes.subarray(0, body.length), w.length);
    w.length += body.length;
    return { serial: s, bytes: w.bytes.subarray(0, w.length) };
  };

  // A whole message from `bytes` (at least its length's worth), or null if not all of it has come yet;
  // { length, m } with m: { type, flags, serial, path, interface, member, error, replySerial, destination,
  // sender, signature, body: [values] }.
  const parse = (bytes) => {
    if (bytes.length < 16) {
      return null;
    }
    const little = bytes[0] === 0x6c;
    if (!little && bytes[0] !== 0x42) {
      throw new Error("dbus: not a message (byte order " + bytes[0] + ")");
    }
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const bodyLength = v.getUint32(4, little);
    const fieldsLength = v.getUint32(12, little);
    const headerEnd = 16 + fieldsLength + ((8 - ((16 + fieldsLength) % 8)) % 8);
    const total = headerEnd + bodyLength;
    if (total > MAX_MESSAGE) {
      throw new Error("dbus: a message of " + total + " bytes");
    }
    if (bytes.length < total) {
      return { need: total };
    }
    const r = new Reader(bytes.subarray(0, total), little);
    r.at = 12;
    const fields = r.get("a(yv)");
    const m = { type: bytes[1], flags: bytes[2], serial: v.getUint32(8, little), body: [] };
    const names = { [F_PATH]: "path", [F_INTERFACE]: "interface", [F_MEMBER]: "member", [F_ERROR]: "error",
                    [F_REPLY_SERIAL]: "replySerial", [F_DESTINATION]: "destination", [F_SENDER]: "sender",
                    [F_SIGNATURE]: "signature" };
    for (const [code, value] of fields) {
      if (names[code]) {
        m[names[code]] = value;
      }
    }
    if (m.signature && bodyLength) {
      r.at = headerEnd;
      m.body = types(m.signature).map((t) => r.get(t));
    }
    return { length: total, m };
  };

  // ---- the connection

  const address = env("DBUS_SESSION_BUS_ADDRESS");
  let fd = -1;
  let inBytes = new Uint8Array(65536), inLength = 0;
  const pending = new Map();	// serial -> { resolve, reject }
  const objects = new Map();	// path -> Map(interface -> spec)
  const matches = [];		// [{ test(m), handler }]
  const owned = new Set();
  let unique = null;
  let queue = [];		// messages to send once we are connected (Hello first)

  const socketPath = (a) => {
    for (const part of (a || "").split(";")) {
      const m = /^unix:(.*)$/.exec(part);
      if (!m) {
        continue;
      }
      const kv = {};
      for (const x of m[1].split(",")) {
        const eq = x.indexOf("=");
        kv[x.slice(0, eq)] = decodeURIComponent(x.slice(eq + 1));
      }
      if (kv.path) {
        return { path: kv.path, abstract: false };
      }
      if (kv.abstract) {
        return { path: kv.abstract, abstract: true };
      }
    }
    return null;
  };

  // Up to `ms` for the socket to have something; then read it. (Only while authenticating: blocking briefly.)
  const readLine = (ms) => {
    const buf = ws.S;
    let line = "";
    const deadline = performance.now() + ms;
    while (!line.endsWith("\r\n")) {
      const v = dv();
      v.setInt32(buf, fd, true);
      v.setInt16(buf + 4, POLLIN, true);
      v.setInt16(buf + 6, 0, true);
      const left = Math.max(0, deadline - performance.now());
      v.setBigInt64(buf + 8, BigInt(Math.floor(left / 1000)), true);
      v.setBigInt64(buf + 16, BigInt(Math.floor((left % 1000) * 1e6)), true);
      const p = sys5(SYS.ppoll, buf, 1, buf + 8, 0, 0);
      if (p === -EINTR) {
        continue;
      }
      if (p <= 0) {
        return null;
      }
      const n = sys3(SYS.read, fd, buf + 32, 1);	// (a byte at a time: nothing after the line is ours yet)
      if (n === -EINTR || n === -EAGAIN) {
        continue;
      }
      if (n <= 0) {
        return null;
      }
      line += String.fromCharCode(u8()[buf + 32]);
      if (line.length > 4096) {
        return null;
      }
    }
    return line.slice(0, -2);
  };

  const connect = (quiet = false, retry = false) => {
    const where = socketPath(address);
    if (!where) {
      log("dbus: no session bus (DBUS_SESSION_BUS_ADDRESS " + JSON.stringify(address) + "): no KDE services here");
      return false;
    }
    fd = sys3(SYS.socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      log("dbus: socket: error " + -fd);
      return false;
    }
    const sa = alloc(112);
    const m = u8();
    m.fill(0, sa, sa + 112);
    dv().setUint16(sa, AF_UNIX, true);
    const raw = encoder.encode(where.path);
    m.set(raw.subarray(0, 106), sa + 2 + (where.abstract ? 1 : 0));
    const r = sys3(SYS.connect, fd, sa, 2 + (where.abstract ? 1 : 0) + raw.length + (where.abstract ? 0 : 1));
    ws.free(sa);
    if (r < 0) {
      if (!quiet) {
        log("dbus: connect " + where.path + ": error " + -r + (retry ? " (tried again every 2 s for a minute)" : ""));
      }
      sys1(SYS.close, fd);
      fd = -1;
      return false;
    }
    // Authentication: a NUL byte, AUTH EXTERNAL with our uid as a string in hex, BEGIN.
    const uid = String(ws.sys0(N_GETUID));
    const hex = [...uid].map((c) => c.charCodeAt(0).toString(16)).join("");
    ws.writeAll(fd, new Uint8Array([0]));
    ws.writeAll(fd, "AUTH EXTERNAL " + hex + "\r\n");
    const answer = readLine(5000);
    if (!answer || !answer.startsWith("OK ")) {
      log("dbus: the bus did not take us (" + JSON.stringify(answer) + "): no KDE services here");
      sys1(SYS.close, fd);
      fd = -1;
      return false;
    }
    ws.writeAll(fd, "BEGIN\r\n");
    sys3(SYS.fcntl, fd, F_SETFL, O_NONBLOCK);
    ws.watchFd(fd, readable);
    return true;
  };

  const write = (bytes) => {
    if (fd < 0) {
      return;
    }
    const r = ws.writeAll(fd, bytes);
    if (r < 0) {
      log("dbus: write: error " + -r + ": the bus is gone");
      gone();
    }
  };

  const gone = () => {
    if (fd >= 0) {
      ws.unwatchFd(fd);
      sys1(SYS.close, fd);
      fd = -1;
    }
    for (const p of pending.values()) {
      p.reject(new Error("the bus is gone"));
    }
    pending.clear();
  };

  const readable = () => {
    for (;;) {
      if (inLength + 65536 > inBytes.length) {
        const bigger = new Uint8Array(inBytes.length * 2);
        bigger.set(inBytes.subarray(0, inLength));
        inBytes = bigger;
      }
      const n = sys3(SYS.read, fd, ws.S, Math.min(ws.SCRATCH, inBytes.length - inLength));
      if (n === -EINTR) {
        continue;
      }
      if (n === -EAGAIN) {
        break;
      }
      if (n <= 0) {
        log("dbus: the bus closed the connection" + (n < 0 ? " (error " + -n + ")" : ""));
        gone();
        return;
      }
      inBytes.set(u8().subarray(ws.S, ws.S + n), inLength);
      inLength += n;
    }
    let at = 0;
    while (at < inLength) {
      let p;
      try {
        p = parse(inBytes.subarray(at, inLength));
      } catch (error) {
        log("dbus: " + error.message + ": the connection is dropped");
        gone();
        return;
      }
      if (!p || p.need) {
        break;
      }
      at += p.length;
      try {
        incoming(p.m);
      } catch (error) {
        log("dbus: handling " + (p.m.interface || "") + "." + (p.m.member || "") + ": " + (error && error.stack || error));
      }
    }
    if (at) {
      inBytes.copyWithin(0, at, inLength);
      inLength -= at;
    }
  };

  const reply = (m, sig, values) => {
    if (m.flags & NO_REPLY) {
      return;
    }
    write(message(T_RETURN, { replySerial: m.serial, destination: m.sender }, sig, values).bytes);
  };
  const replyError = (m, name, text) => {
    if (m.flags & NO_REPLY) {
      return;
    }
    write(message(T_ERROR, { replySerial: m.serial, destination: m.sender, error: name }, "s", [text || name]).bytes);
  };

  const incoming = (m) => {
    if (debug) {
      log("dbus: < " + ["", "call", "return", "error", "signal"][m.type] + " " + (m.sender || "") + " " + (m.path || "")
          + " " + (m.interface || "") + "." + (m.member || "") + (m.replySerial ? " re " + m.replySerial : "")
          + " (" + (m.signature || "") + ") " + JSON.stringify(m.body, (k, v) => (v instanceof Uint8Array ? "<" + v.length + " bytes>" : v)).slice(0, 300));
    }
    if (m.type === T_RETURN || m.type === T_ERROR) {
      const p = pending.get(m.replySerial);
      if (p) {
        pending.delete(m.replySerial);
        if (m.type === T_RETURN) {
          p.resolve(m.body);
        } else {
          p.reject(new Fail(m.error, m.body[0]));
        }
      }
      return;
    }
    if (m.type === T_SIGNAL) {
      if (m.interface === "org.freedesktop.DBus" && m.member === "NameAcquired") {
        owned.add(m.body[0]);
      } else if (m.interface === "org.freedesktop.DBus" && m.member === "NameLost") {
        owned.delete(m.body[0]);
        log("dbus: lost the name " + m.body[0]);
      }
      for (const x of matches) {
        if (x.test(m)) {
          x.handler(m);
        }
      }
      return;
    }
    if (m.type === T_CALL) {
      call(m);
    }
  };

  // ---- the objects we export

  const exported = (path) => objects.get(path) || null;

  const childrenOf = (path) => {
    const prefix = path === "/" ? "/" : path + "/";
    const names = new Set();
    for (const p of objects.keys()) {
      if (p.startsWith(prefix) && p !== path) {
        names.add(p.slice(prefix.length).split("/")[0]);
      }
    }
    return [...names];
  };

  const xmlEscape = (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");

  const introspect = (path) => {
    const out = ['<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"',
                 '"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">', "<node>"];
    const ifaces = exported(path);
    if (ifaces) {
      out.push('  <interface name="org.freedesktop.DBus.Introspectable"><method name="Introspect"><arg name="xml_data" type="s" direction="out"/></method></interface>');
      out.push('  <interface name="org.freedesktop.DBus.Peer"><method name="Ping"/><method name="GetMachineId"><arg name="machine_uuid" type="s" direction="out"/></method></interface>');
      out.push('  <interface name="org.freedesktop.DBus.Properties">'
               + '<method name="Get"><arg name="interface_name" type="s" direction="in"/><arg name="property_name" type="s" direction="in"/><arg name="value" type="v" direction="out"/></method>'
               + '<method name="GetAll"><arg name="interface_name" type="s" direction="in"/><arg name="values" type="a{sv}" direction="out"/></method>'
               + '<method name="Set"><arg name="interface_name" type="s" direction="in"/><arg name="property_name" type="s" direction="in"/><arg name="value" type="v" direction="in"/></method>'
               + '<signal name="PropertiesChanged"><arg name="interface_name" type="s"/><arg name="changed_properties" type="a{sv}"/><arg name="invalidated_properties" type="as"/></signal>'
               + "</interface>");
      for (const [name, spec] of ifaces) {
        out.push('  <interface name="' + xmlEscape(name) + '">');
        for (const [pname, p] of Object.entries(spec.properties || {})) {
          out.push('    <property name="' + xmlEscape(pname) + '" type="' + p.type + '" access="' + (p.set ? "readwrite" : "read") + '"/>');
        }
        for (const [mname, me] of Object.entries(spec.methods || {})) {
          out.push('    <method name="' + xmlEscape(mname) + '">');
          types(me.in || "").forEach((t, i) => out.push('      <arg name="' + xmlEscape((me.inNames || [])[i] || "arg" + i) + '" type="' + t + '" direction="in"/>'));
          types(me.out || "").forEach((t, i) => out.push('      <arg name="' + xmlEscape((me.outNames || [])[i] || "out" + i) + '" type="' + t + '" direction="out"/>'));
          for (const [k, val] of Object.entries(me.annotations || {})) {
            out.push('      <annotation name="' + xmlEscape(k) + '" value="' + xmlEscape(val) + '"/>');
          }
          if (me.noReply) {
            out.push('      <annotation name="org.freedesktop.DBus.Method.NoReply" value="true"/>');
          }
          out.push("    </method>");
        }
        for (const [sname, sig] of Object.entries(spec.signals || {})) {
          const s = typeof sig === "string" ? { type: sig } : sig;
          out.push('    <signal name="' + xmlEscape(sname) + '">');
          types(s.type || "").forEach((t, i) => out.push('      <arg name="' + xmlEscape((s.names || [])[i] || "arg" + i) + '" type="' + t + '"/>'));
          for (const [k, val] of Object.entries(s.annotations || {})) {
            out.push('      <annotation name="' + xmlEscape(k) + '" value="' + xmlEscape(val) + '"/>');
          }
          out.push("    </signal>");
        }
        out.push("  </interface>");
      }
    }
    for (const child of childrenOf(path)) {
      out.push('  <node name="' + xmlEscape(child) + '"/>');
    }
    out.push("</node>");
    return out.join("\n");
  };

  const machineId = () => (ws.readText("/etc/machine-id") || ws.readText("/var/lib/dbus/machine-id") || "0".repeat(32)).trim();

  const call = (m) => {
    const path = m.path || "/";
    const iface = m.interface;
    const member = m.member;
    if (iface === "org.freedesktop.DBus.Introspectable" || (!iface && member === "Introspect")) {
      if (member === "Introspect") {
        reply(m, "s", [introspect(path)]);
        return;
      }
    }
    if (iface === "org.freedesktop.DBus.Peer") {
      if (member === "Ping") {
        reply(m, "", []);
      } else if (member === "GetMachineId") {
        reply(m, "s", [machineId()]);
      } else {
        replyError(m, "org.freedesktop.DBus.Error.UnknownMethod", "no method " + member);
      }
      return;
    }
    const ifaces = exported(path);
    if (!ifaces) {
      replyError(m, "org.freedesktop.DBus.Error.UnknownObject", "no object at " + path);
      return;
    }
    if (iface === "org.freedesktop.DBus.Properties") {
      properties(m, ifaces);
      return;
    }
    // A call without an interface: the first interface with that method.
    let spec = iface ? ifaces.get(iface) : null;
    if (!iface) {
      for (const s of ifaces.values()) {
        if (s.methods && s.methods[member]) {
          spec = s;
          break;
        }
      }
    }
    const method = spec && spec.methods && spec.methods[member];
    if (!method) {
      replyError(m, spec ? "org.freedesktop.DBus.Error.UnknownMethod" : "org.freedesktop.DBus.Error.UnknownInterface",
                 "no method " + member + " in " + (iface || "any interface") + " at " + path);
      return;
    }
    if ((m.signature || "") !== (method.in || "")) {
      replyError(m, "org.freedesktop.DBus.Error.InvalidArgs", member + " takes (" + (method.in || "") + "), not (" + (m.signature || "") + ")");
      return;
    }
    let result;
    try {
      result = method.fn(m.body, m);
    } catch (error) {
      if (error instanceof Fail) {
        replyError(m, error.dbusName, error.message);
      } else {
        log("dbus: " + member + ": " + (error && error.stack || error));
        replyError(m, "org.freedesktop.DBus.Error.Failed", String(error && error.message || error));
      }
      return;
    }
    const outs = types(method.out || "");
    const answer = (value) => reply(m, method.out || "", outs.length === 0 ? [] : outs.length === 1 ? [value] : value);
    if (result instanceof Promise) {
      // (a reply that comes later: the window a person picks, say)
      result.then(answer, (error) => replyError(m, error instanceof Fail ? error.dbusName : "org.freedesktop.DBus.Error.Failed",
                                                String(error && error.message || error)));
      return;
    }
    answer(result);
  };

  const properties = (m, ifaces) => {
    const [ifaceName, name, value] = m.body;
    const spec = ifaces.get(ifaceName);
    const props = (spec && spec.properties) || {};
    try {
      if (m.member === "Get" && m.signature === "ss") {
        const p = props[name];
        if (!p) {
          throw new Fail("org.freedesktop.DBus.Error.UnknownProperty", "no property " + name);
        }
        reply(m, "v", [new V(p.type, p.get())]);
      } else if (m.member === "GetAll" && m.signature === "s") {
        const all = [];
        for (const [k, p] of Object.entries(props)) {
          all.push([k, new V(p.type, p.get())]);
        }
        reply(m, "a{sv}", [all]);
      } else if (m.member === "Set" && m.signature === "ssv") {
        const p = props[name];
        if (!p) {
          throw new Fail("org.freedesktop.DBus.Error.UnknownProperty", "no property " + name);
        }
        if (!p.set) {
          throw new Fail("org.freedesktop.DBus.Error.PropertyReadOnly", name + " is read-only");
        }
        p.set(value);
        reply(m, "", []);
      } else {
        throw new Fail("org.freedesktop.DBus.Error.UnknownMethod", "no method " + m.member + " (" + m.signature + ")");
      }
    } catch (error) {
      if (error instanceof Fail) {
        replyError(m, error.dbusName, error.message);
      } else {
        log("dbus: properties: " + (error && error.stack || error));
        replyError(m, "org.freedesktop.DBus.Error.Failed", String(error && error.message || error));
      }
    }
  };

  // ---- what the other parts use

  const send = (bytes) => {
    if (unique === null) {
      queue.push(bytes);	// (before Hello's answer: nothing may go before Hello)
    } else {
      write(bytes);
    }
  };

  const callBus = (destination, path, iface, member, sig, values, hello = false) => {
    if (fd < 0) {
      return Promise.reject(new Error("no bus"));
    }
    const msg = message(T_CALL, { destination, path, interface: iface, member }, sig, values || []);
    const promise = new Promise((resolve, reject) => pending.set(msg.serial, { resolve, reject }));
    if (hello) {
      write(msg.bytes);
    } else {
      send(msg.bytes);
    }
    return promise;
  };

  const dbus = {
    V, Fail,
    get connected() {
      return fd >= 0;
    },
    get name() {
      return unique;
    },
    owns: (name) => owned.has(name),
    export(path, iface, spec) {
      if (!objects.has(path)) {
        objects.set(path, new Map());
      }
      objects.get(path).set(iface, spec);
    },
    unexport(path, iface) {
      const o = objects.get(path);
      if (o) {
        o.delete(iface);
        if (!o.size) {
          objects.delete(path);
        }
      }
    },
    emit(path, iface, member, sig, values, destination) {
      if (fd < 0) {
        return;
      }
      if (debug) {
        log("dbus: > signal " + path + " " + iface + "." + member + " " + JSON.stringify(values || []).slice(0, 200));
      }
      send(message(T_SIGNAL, { path, interface: iface, member, destination }, sig || "", values || []).bytes);
    },
    // PropertiesChanged for some properties of an exported interface (their values read now).
    changed(path, iface, names) {
      const spec = exported(path) && exported(path).get(iface);
      if (!spec) {
        return;
      }
      const props = spec.properties || {};
      dbus.emit(path, "org.freedesktop.DBus.Properties", "PropertiesChanged", "sa{sv}as",
                [iface, names.filter((n) => props[n]).map((n) => [n, new V(props[n].type, props[n].get())]), []]);
    },
    call: (destination, path, iface, member, sig, values) => callBus(destination, path, iface, member, sig, values),
    own(name) {
      // 2 replace an owner that allows it, 4 do not queue: an answer of 1 (primary owner) or 4 (had it already)
      return callBus("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", [name, 6])
        .then(([r]) => {
          if (r === 1 || r === 4) {
            owned.add(name);
            log("dbus: " + name + " is ours");
            return true;
          }
          log("dbus: " + name + " is another program's (RequestName said " + r + ")");
          return false;
        }, (error) => {
          log("dbus: RequestName " + name + ": " + error.message);
          return false;
        });
    },
    // rule: { type, sender, interface, member, path, arg0 }, each optional; handler(message).
    match(rule, handler) {
      const text = Object.entries(rule).map(([k, v]) => k + "='" + String(v).replace(/'/g, "") + "'").join(",");
      const test = (m) => (!rule.type || rule.type === "signal") &&
        (!rule.interface || m.interface === rule.interface) && (!rule.member || m.member === rule.member) &&
        (!rule.path || m.path === rule.path) && (!rule.sender || m.sender === rule.sender ||
          (rule.sender === "org.freedesktop.DBus" && m.sender === "org.freedesktop.DBus")) &&
        (rule.arg0 === undefined || m.body[0] === rule.arg0);
      matches.push({ test, handler });
      callBus("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s", [text])
        .catch((error) => log("dbus: AddMatch " + text + ": " + error.message));
    },
    // For tests: the wire format both ways.
    marshal: (sig, values) => {
      const w = new Writer();
      types(sig).forEach((t, i) => w.put(t, values[i]));
      return w.bytes.slice(0, w.length);
    },
    unmarshal: (sig, bytes) => {
      const r = new Reader(bytes, true);
      return types(sig).map((t) => r.get(t));
    },
    message, parse,
  };
  ws.dbus = dbus;

  const hello = () => {
    callBus("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", "", [], true)
      .then(([name]) => {
        unique = name;
        log("dbus: on the session bus as " + name);
        const q = queue;
        queue = [];
        for (const bytes of q) {
          write(bytes);
        }
      }, (error) => log("dbus: Hello: " + error.message));
  };
  // A bus that is not there yet (its daemon started after us) is tried again every 2 s for a minute; what the
  // services send in the meantime waits in the queue (nothing goes before Hello).
  const RETRIES = 30;
  let tries = 0;
  const start = () => {
    if (connect(tries > 0, true)) {
      if (tries > 0) {
        log("dbus: the session bus is there now");
      }
      hello();
    } else if (socketPath(address) && ++tries < RETRIES) {
      setTimeout(start, 2000);
    } else if (tries) {
      log("dbus: still no session bus: no KDE services here");
    }
  };
  if (address) {
    start();
  } else {
    log("dbus: no DBUS_SESSION_BUS_ADDRESS: no KDE services here");
  }
})
