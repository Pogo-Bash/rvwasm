// The X11 protocol, the client's side, as much of it as the window manager
// (xwm.js) and the clipboard bridge (xselection.js) need: the connection's
// setup, the requests they make, and the events, replies and errors that
// come back. Written from the X Window System Protocol document (X11R7,
// "X Window System Protocol, Version 11"), and for the two extensions used,
// the XFIXES and Composite extension protocol documents.
//
// The wire, in short (we always speak little-endian, byte order 'l'):
//   a request is 4 bytes of header (major opcode, one byte of data, the
//     request's length in 4-byte words, header included) and its body, padded
//     to 4 bytes; an extension's requests have the extension's major opcode
//     and the request's minor opcode in the data byte;
//   what comes back is 32 bytes at a time: an error (first byte 0), a reply
//     (1; a reply's word at 4 says how many more 4-byte words follow), or an
//     event (2 and up; the top bit set if another client sent it with
//     SendEvent; GenericEvent, 35, has more words like a reply). Each carries
//     the low 16 bits of the sequence number of the last request the server
//     has taken, which is how replies and errors are matched to requests.
//
// Everything here is non-blocking on the read side: the socket is watched by
// the server's epoll (server.js: watchFd), and what has come is decoded and
// handed out. Writes go out at once (ws.writeAll: a full socket is waited
// for, which with Xwayland, which reads promptly, is short).
(function (ws) {
  "use strict";

  const { sys1, sys3, SYS, u8, dv, alloc, log } = ws;
  const EINTR = 4, EAGAIN = 11;

  // Core request opcodes (the protocol document's numbers).
  const OP = {
    CreateWindow: 1, ChangeWindowAttributes: 2, GetWindowAttributes: 3, DestroyWindow: 4, MapWindow: 8,
    UnmapWindow: 10, ConfigureWindow: 12, GetGeometry: 14, QueryTree: 15, InternAtom: 16, GetAtomName: 17,
    ChangeProperty: 18, DeleteProperty: 19, GetProperty: 20, SetSelectionOwner: 22, GetSelectionOwner: 23,
    ConvertSelection: 24, SendEvent: 25, SetInputFocus: 42, QueryExtension: 98, KillClient: 113,
  };
  // Event codes.
  const EV = {
    KeyPress: 2, FocusIn: 9, FocusOut: 10, CreateNotify: 16, DestroyNotify: 17, UnmapNotify: 18, MapNotify: 19,
    MapRequest: 20, ReparentNotify: 21, ConfigureNotify: 22, ConfigureRequest: 23, PropertyNotify: 28,
    SelectionClear: 29, SelectionRequest: 30, SelectionNotify: 31, ClientMessage: 33, GenericEvent: 35,
  };
  // Event masks (for ChangeWindowAttributes' event-mask and SendEvent).
  const MASK = {
    NoEvent: 0, StructureNotify: 0x20000, SubstructureNotify: 0x80000, SubstructureRedirect: 0x100000,
    FocusChange: 0x200000, PropertyChange: 0x400000,
  };
  // ChangeWindowAttributes' value bits (in the order their values go).
  const CW = { BackPixel: 0x2, OverrideRedirect: 0x200, EventMask: 0x800 };
  // ConfigureWindow's value bits (in this order too).
  const CONFIG = { X: 1, Y: 2, Width: 4, Height: 8, BorderWidth: 16, Sibling: 32, StackMode: 64 };
  const ERRORS = ["", "Request", "Value", "Window", "Pixmap", "Atom", "Cursor", "Font", "Match", "Drawable", "Access",
                  "Alloc", "Colormap", "GContext", "IDChoice", "Name", "Length", "Implementation"];

  const encoder = new TextEncoder();
  const latin1 = new TextDecoder("latin1");
  const utf8 = new TextDecoder("utf-8");

  // A request's bytes as they are made.
  class Writer {
    constructor() {
      this.bytes = new Uint8Array(256);
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
    u8(v) {
      this.room(1);
      this.view.setUint8(this.length, v & 0xff);
      this.length += 1;
      return this;
    }
    u16(v) {
      this.room(2);
      this.view.setUint16(this.length, v & 0xffff, true);
      this.length += 2;
      return this;
    }
    i16(v) {
      this.room(2);
      this.view.setInt16(this.length, v, true);
      this.length += 2;
      return this;
    }
    u32(v) {
      this.room(4);
      this.view.setUint32(this.length, v >>> 0, true);
      this.length += 4;
      return this;
    }
    i32(v) {
      this.room(4);
      this.view.setInt32(this.length, v | 0, true);
      this.length += 4;
      return this;
    }
    raw(data) {
      this.room(data.length);
      this.bytes.set(data, this.length);
      this.length += data.length;
      return this;
    }
    pad() {
      while (this.length & 3) {
        this.u8(0);
      }
      return this;
    }
  }

  // What came back: an event, a reply or an error, as a copy with a DataView over it.
  class Message {
    constructor(bytes) {
      this.bytes = bytes;
      this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    }
    u8(at) {
      return this.view.getUint8(at);
    }
    u16(at) {
      return this.view.getUint16(at, true);
    }
    i16(at) {
      return this.view.getInt16(at, true);
    }
    u32(at) {
      return this.view.getUint32(at, true);
    }
    i32(at) {
      return this.view.getInt32(at, true);
    }
  }

  // A connection to an X server over a socket we already have (the window manager's socket that Xwayland was
  // given with -wm). onEvent(message) for every event, onClose() when the server is gone.
  class Connection {
    constructor(fd, onEvent, onClose) {
      this.fd = fd;
      this.onEvent = onEvent;
      this.onClose = onClose;
      this.seq = 0;			// requests sent (the server's count of what it has taken is in what comes back)
      this.pending = new Map();	// sequence (low 16 bits) -> { reply(message|null, error|null), quiet }
      this.inBytes = new Uint8Array(65536);
      this.inLength = 0;
      this.readBuf = alloc(65536);
      this.ready = false;
      this.closed = false;
      this.setup = null;
      this.nextId = 0;
      this.extensions = {};	// name -> { opcode, event, error }
      this.time = 0;		// the newest server time seen in an event (for requests that want a real one)
    }

    // ---- the setup

    // Say who we are; onReady(setup) once the server has answered.
    start(onReady) {
      this.onReady = onReady;
      const w = new Writer();
      w.u8(0x6c).u8(0).u16(11).u16(0).u16(0).u16(0).u16(0);	// 'l', protocol 11.0, no authorization
      this.write(w.bytes.subarray(0, w.length));
    }

    parseSetup(b) {
      const m = new Message(b);
      if (m.u8(0) !== 1) {
        const why = latin1.decode(b.subarray(8, 8 + m.u8(1)));
        log("x11: the X server refused us: " + why);
        this.close();
        return;
      }
      const vendorLength = m.u16(24), formats = m.u8(29);
      let at = 40 + ((vendorLength + 3) & ~3) + 8 * formats;
      // The first screen (Xwayland has one).
      const screen = {
        root: m.u32(at), colormap: m.u32(at + 4), white: m.u32(at + 8), black: m.u32(at + 12),
        width: m.u16(at + 20), height: m.u16(at + 22), rootVisual: m.u32(at + 32), rootDepth: m.u8(at + 38),
      };
      this.setup = {
        idBase: m.u32(12), idMask: m.u32(16), maxRequest: m.u16(26), screen,
        minKeycode: m.u8(34), maxKeycode: m.u8(35),
      };
      this.idShift = 0;
      while (!((this.setup.idMask >>> this.idShift) & 1)) {
        this.idShift++;
      }
      this.ready = true;
      const f = this.onReady;
      this.onReady = null;
      f(this.setup);
    }

    // A new resource id of ours.
    id() {
      const s = this.setup;
      return (s.idBase | ((++this.nextId << this.idShift) & s.idMask)) >>> 0;
    }

    // ---- requests

    // Send a request: major opcode, the data byte, a function that writes the body (or null), and what to do
    // with its reply (a function of (message, error), or null for a request without one). Errors of requests
    // with no reply function are logged, unless `quiet` (windows that may be gone by now: BadWindow is normal).
    request(opcode, data, body, reply, quiet) {
      if (this.closed) {
        if (reply) {
          reply(null, { code: 0, name: "closed" });
        }
        return 0;
      }
      const w = new Writer();
      w.u8(opcode).u8(data).u16(0);
      if (body) {
        body(w);
      }
      w.pad();
      const words = w.length >> 2;
      if (words > 0xffff) {
        log("x11: a request of " + w.length + " bytes is too long (opcode " + opcode + ")");
        return 0;
      }
      w.view.setUint16(2, words, true);
      this.seq = (this.seq + 1) & 0xffff;
      if (reply || quiet) {
        this.pending.set(this.seq, { reply, quiet, opcode });
      }
      this.write(w.bytes.subarray(0, w.length));
      return this.seq;
    }

    // An extension's request (its major opcode from QueryExtension, the request's minor opcode in the data byte).
    ext(name, minor, body, reply, quiet) {
      const e = this.extensions[name];
      if (!e) {
        if (reply) {
          reply(null, { code: 0, name: "no extension " + name });
        }
        return 0;
      }
      return this.request(e.opcode, minor, body, reply, quiet);
    }

    write(bytes) {
      if (this.closed) {
        return;
      }
      if (ws.writeAll(this.fd, bytes) < 0) {
        this.close();
      }
    }

    // ---- what comes back

    // The socket has something (server.js calls this from its epoll).
    readable() {
      for (;;) {
        const n = sys3(SYS.read, this.fd, this.readBuf, 65536);
        if (n === -EINTR) {
          continue;
        }
        if (n === -EAGAIN) {
          break;
        }
        if (n <= 0) {
          this.close();
          return;
        }
        if (this.inLength + n > this.inBytes.length) {
          const bigger = new Uint8Array(Math.max(this.inBytes.length * 2, this.inLength + n));
          bigger.set(this.inBytes.subarray(0, this.inLength));
          this.inBytes = bigger;
        }
        this.inBytes.set(u8().subarray(this.readBuf, this.readBuf + n), this.inLength);
        this.inLength += n;
        if (n < 65536) {
          break;
        }
      }
      this.decode();
    }

    decode() {
      let at = 0;
      const b = this.inBytes;
      const v = new DataView(b.buffer);
      while (!this.closed) {
        const left = this.inLength - at;
        if (!this.ready) {
          if (left < 8) {
            break;
          }
          const size = 8 + 4 * v.getUint16(at + 6, true);
          if (left < size) {
            break;
          }
          const setup = b.slice(at, at + size);
          at += size;
          this.parseSetup(setup);
          continue;
        }
        if (left < 32) {
          break;
        }
        const type = b[at];
        let size = 32;
        if (type === 1 || (type & 0x7f) === EV.GenericEvent) {
          size += 4 * v.getUint32(at + 4, true);
        }
        if (left < size) {
          break;
        }
        const m = new Message(b.slice(at, at + size));
        at += size;
        if (type === 0) {
          this.error(m);
        } else if (type === 1) {
          const seq = m.u16(2);
          const p = this.pending.get(seq);
          this.pending.delete(seq);
          if (p && p.reply) {
            p.reply(m, null);
          }
        } else {
          this.event(m);
        }
      }
      if (at > 0) {
        b.copyWithin(0, at, this.inLength);
        this.inLength -= at;
      }
    }

    error(m) {
      const code = m.u8(1), seq = m.u16(2);
      const e = { code, name: ERRORS[code] || "error " + code, value: m.u32(4), minor: m.u16(8), major: m.u8(10) };
      const p = this.pending.get(seq);
      this.pending.delete(seq);
      if (p && p.reply) {
        p.reply(null, e);
        return;
      }
      if (!p || !p.quiet || ws.xdebug) {
        log("x11: Bad" + e.name + " (value " + e.value + ") for request " + e.major + "." + e.minor + " (sequence " + seq + ")");
      }
    }

    event(m) {
      // The newest server time: events that carry one have it at offset 4 or 12 (only the kinds we look at).
      const code = m.u8(0) & 0x7f;
      if (code === EV.PropertyNotify) {
        this.time = m.u32(12);
      } else if (code === EV.SelectionRequest || code === EV.SelectionClear || code === EV.SelectionNotify) {
        if (m.u32(4)) {
          this.time = m.u32(4);
        }
      }
      // Replies to requests that had none: the sequence numbers we kept for errors only are forgotten as the
      // server passes them.
      if (this.pending.size > 256) {
        const seq = m.u16(2);
        for (const k of this.pending.keys()) {
          if (((seq - k) & 0xffff) < 0x8000 && !this.pending.get(k).reply) {
            this.pending.delete(k);
          }
        }
      }
      this.onEvent(m);
    }

    close() {
      if (this.closed) {
        return;
      }
      this.closed = true;
      for (const p of this.pending.values()) {
        if (p.reply) {
          try {
            p.reply(null, { code: 0, name: "closed" });
          } catch (error) {
            log("x11: " + error);
          }
        }
      }
      this.pending.clear();
      ws.free(this.readBuf);
      if (this.onClose) {
        this.onClose();
      }
    }

    // ---- the requests used, by name

    internAtoms(names, done) {
      const atoms = {};
      let left = names.length;
      for (const name of names) {
        const raw = encoder.encode(name);
        this.request(OP.InternAtom, 0, (w) => w.u16(raw.length).u16(0).raw(raw), (m) => {
          atoms[name] = m ? m.u32(8) : 0;
          if (--left === 0) {
            done(atoms);
          }
        });
      }
    }

    getAtomName(atom, done) {
      this.request(OP.GetAtomName, 0, (w) => w.u32(atom), (m) => {
        done(m ? latin1.decode(m.bytes.subarray(32, 32 + m.u16(8))) : null);
      });
    }

    queryExtension(name, done) {
      const raw = encoder.encode(name);
      this.request(OP.QueryExtension, 0, (w) => w.u16(raw.length).u16(0).raw(raw), (m) => {
        if (m && m.u8(8)) {
          this.extensions[name] = { opcode: m.u8(9), event: m.u8(10), error: m.u8(11) };
        }
        done(this.extensions[name] || null);
      });
    }

    // CreateWindow: an InputOnly window (no pixels) of ours, for properties and selections.
    createInputOnly(parent, x, y, w, h, eventMask) {
      const wid = this.id();
      this.request(OP.CreateWindow, 0, (b) => {
        b.u32(wid).u32(parent).i16(x).i16(y).u16(w).u16(h).u16(0).u16(2).u32(0);	// class InputOnly, visual from parent
        b.u32(CW.OverrideRedirect | CW.EventMask).u32(1).u32(eventMask);
      });
      return wid;
    }

    changeWindowAttributes(window, mask, values, quiet = true) {
      this.request(OP.ChangeWindowAttributes, 0, (w) => {
        w.u32(window).u32(mask);
        for (const v of values) {
          w.u32(v);
        }
      }, null, quiet);
    }

    selectInput(window, mask) {
      this.changeWindowAttributes(window, CW.EventMask, [mask]);
    }

    mapWindow(window) {
      this.request(OP.MapWindow, 0, (w) => w.u32(window), null, true);
    }

    // ConfigureWindow with the values given ({ x, y, width, height, border, sibling, stack }; stack: 0 Above,
    // 1 Below).
    configureWindow(window, values) {
      let mask = 0;
      const list = [];
      const put = (bit, v) => {
        if (v !== undefined) {
          mask |= bit;
          list.push(v);
        }
      };
      put(CONFIG.X, values.x);
      put(CONFIG.Y, values.y);
      put(CONFIG.Width, values.width);
      put(CONFIG.Height, values.height);
      put(CONFIG.BorderWidth, values.border);
      put(CONFIG.Sibling, values.sibling);
      put(CONFIG.StackMode, values.stack);
      if (!mask) {
        return;
      }
      this.request(OP.ConfigureWindow, 0, (w) => {
        w.u32(window).u16(mask).u16(0);
        list.forEach((v, i) => {
          // (x and y are INT16 sent as 32 bits: sign-extended)
          w.i32(v);
        });
      }, null, true);
    }

    // ChangeProperty: format 8 (a Uint8Array or a string, sent as UTF-8), 32 (an array of numbers). mode: 0
    // replace, 2 append.
    changeProperty(window, property, type, format, data, mode = 0) {
      let raw, count;
      if (format === 8) {
        raw = typeof data === "string" ? encoder.encode(data) : data;
        count = raw.length;
      } else {
        const a = new Uint8Array(4 * data.length);
        const v = new DataView(a.buffer);
        data.forEach((x, i) => v.setUint32(4 * i, x >>> 0, true));
        raw = a;
        count = data.length;
      }
      this.request(OP.ChangeProperty, mode, (w) => {
        w.u32(window).u32(property).u32(type).u8(format).u8(0).u16(0).u32(count).raw(raw);
      }, null, true);
    }

    deleteProperty(window, property) {
      this.request(OP.DeleteProperty, 0, (w) => w.u32(window).u32(property), null, true);
    }

    // GetProperty: done({ type, format, data: Uint8Array, after }) or done(null) if there is none (or the window
    // is gone). `max` in bytes.
    getProperty(window, property, done, { del = false, type = 0, max = 1 << 20 } = {}) {
      this.request(OP.GetProperty, del ? 1 : 0, (w) => {
        w.u32(window).u32(property).u32(type).u32(0).u32(Math.ceil(max / 4));
      }, (m) => {
        if (!m || m.u32(8) === 0) {
          done(null);
          return;
        }
        const format = m.u8(1), units = m.u32(16);
        const bytes = format === 8 ? units : format === 16 ? 2 * units : 4 * units;
        done({ type: m.u32(8), format, after: m.u32(12), data: m.bytes.slice(32, 32 + bytes) });
      });
    }

    setSelectionOwner(owner, selection, time = 0) {
      this.request(OP.SetSelectionOwner, 0, (w) => w.u32(owner).u32(selection).u32(time));
    }

    convertSelection(requestor, selection, target, property, time = 0) {
      this.request(OP.ConvertSelection, 0, (w) => w.u32(requestor).u32(selection).u32(target).u32(property).u32(time));
    }

    // SendEvent of 32 bytes made by `fill(writer)` (which writes all of them).
    sendEvent(destination, propagate, mask, fill) {
      this.request(OP.SendEvent, propagate ? 1 : 0, (w) => {
        w.u32(destination).u32(mask);
        const start = w.length;
        fill(w);
        while (w.length < start + 32) {
          w.u8(0);
        }
      }, null, true);
    }

    // A ClientMessage of format 32 to `window` (five words of data).
    clientMessage(window, type, data, mask = 0, destination = window) {
      this.sendEvent(destination, false, mask, (w) => {
        w.u8(EV.ClientMessage).u8(32).u16(0).u32(window).u32(type);
        for (let i = 0; i < 5; i++) {
          w.u32(data[i] || 0);
        }
      });
    }

    setInputFocus(window, revertTo = 1, time = 0) {
      this.request(OP.SetInputFocus, revertTo, (w) => w.u32(window).u32(time), null, true);
    }

    killClient(resource) {
      this.request(OP.KillClient, 0, (w) => w.u32(resource), null, true);
    }
  }

  // Text in a property: UTF-8 if its type says so, else Latin-1 (STRING; COMPOUND_TEXT read as Latin-1 too,
  // which is right for its ASCII part).
  const propertyText = (p, utf8Type) => {
    if (!p || p.format !== 8) {
      return null;
    }
    let data = p.data;
    const nul = data.indexOf(0);
    if (nul >= 0) {
      data = data.subarray(0, nul);
    }
    return p.type === utf8Type ? utf8.decode(data) : latin1.decode(data);
  };

  // A property's 32-bit values.
  const propertyWords = (p) => {
    if (!p || p.format !== 32) {
      return [];
    }
    const v = new DataView(p.data.buffer, p.data.byteOffset, p.data.byteLength);
    const list = [];
    for (let i = 0; i + 4 <= p.data.length; i += 4) {
      list.push(v.getUint32(i, true));
    }
    return list;
  };

  ws.X11 = { Connection, Writer, OP, EV, MASK, CW, CONFIG, propertyText, propertyWords };
})
