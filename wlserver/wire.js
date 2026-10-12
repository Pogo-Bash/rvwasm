// The wire: Wayland messages in and out of a client's socket, by the tables
// of protocol.js.
//
// A message is the id of the object it is for (32 bits), then its size in
// bytes and its opcode (16 bits each, size in the upper half), then its
// arguments, each padded to 32 bits: integers as they are, fixed-point
// numbers as 24.8, strings and arrays as a length and the bytes (a string's
// length counts its final NUL), objects and new ids as ids. A file
// descriptor is not in the message at all: it comes beside it, as ancillary
// data of the sendmsg that carried the message (SCM_RIGHTS), and the
// receiver pairs descriptors with the "h" arguments of the messages in the
// order they arrived. (Guest descriptors are the kernel's own numbers here:
// rv passes the ancillary data through unchanged, so what a riscv64 client
// sent is what this process gets.)
//
// This file knows nothing of what the messages mean: compositor.js does.
(function (ws) {
  "use strict";

  const { sys1, sys3, sys4, SYS, dv, u8, S, SCRATCH, protocol, log } = ws;
  const EAGAIN = 11, EINTR = 4;
  const MSG_DONTWAIT = 0x40, MSG_CMSG_CLOEXEC = 0x40000000, MSG_NOSIGNAL = 0x4000;
  const SOL_SOCKET = 1, SCM_RIGHTS = 1;
  const MAX_FDS = 28;		// libwayland's own limit per message

  // The scratch block is laid out for recvmsg and sendmsg: the message header, one iovec, room for the
  // ancillary data, and the bytes.
  const MSG = S;			// struct msghdr (56 bytes)
  const IOV = S + 64;		// struct iovec (16 bytes)
  const CTL = S + 96;		// cmsghdr (16) + MAX_FDS descriptors (4 each), 8-aligned
  const CTL_SIZE = 16 + 4 * MAX_FDS;
  const DATA = S + 256;
  const DATA_SIZE = SCRATCH - 256;

  const opcodeOf = {};		// interface -> event name -> opcode
  for (const name of Object.keys(protocol)) {
    const map = {};
    protocol[name].events.forEach((e, i) => {
      map[e.name] = i;
    });
    opcodeOf[name] = map;
  }

  const encoder = new TextEncoder();
  const decoder = new TextDecoder("utf-8");

  // Outgoing bytes of a client, grown as needed; flushed by turn's end or when full.
  class Outgoing {
    constructor() {
      this.bytes = new Uint8Array(16384);
      this.view = new DataView(this.bytes.buffer);
      this.length = 0;
      this.fds = [];
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
  }

  // Put one message for `object` into its client's outgoing bytes. Arguments come as JavaScript values:
  // numbers for i/u/f/o/n (an object argument may be given as the object itself), strings (or null), Uint8Array
  // or ArrayBuffer for arrays, and a descriptor number for h.
  ws.send = (object, name, ...values) => {
    const client = object.client;
    if (!client || client.gone) {
      return;
    }
    const iface = protocol[object.interface];
    const opcode = opcodeOf[object.interface][name];
    if (opcode === undefined) {
      throw new Error("wire: no event " + object.interface + "." + name);
    }
    const event = iface.events[opcode];
    if (event.since > object.version) {
      return;	// the client's version of the object has no such event
    }
    const out = client.out;
    const start = out.length;
    out.room(8);
    out.length += 8;
    let v = 0;
    for (const arg of event.args) {
      const type = arg[1].replace("?", "");
      const value = values[v++];
      if (type === "s") {
        if (value === null || value === undefined) {
          out.room(4);
          out.view.setUint32(out.length, 0, true);
          out.length += 4;
        } else {
          const raw = encoder.encode(value);
          const padded = (raw.length + 1 + 3) & ~3;
          out.room(4 + padded);
          out.view.setUint32(out.length, raw.length + 1, true);
          out.bytes.set(raw, out.length + 4);
          out.bytes.fill(0, out.length + 4 + raw.length, out.length + 4 + padded);
          out.length += 4 + padded;
        }
      } else if (type === "a") {
        const raw = value instanceof Uint8Array ? value : new Uint8Array(value || 0);
        const padded = (raw.length + 3) & ~3;
        out.room(4 + padded);
        out.view.setUint32(out.length, raw.length, true);
        out.bytes.set(raw, out.length + 4);
        out.bytes.fill(0, out.length + 4 + raw.length, out.length + 4 + padded);
        out.length += 4 + padded;
      } else if (type === "h") {
        out.fds.push(value);
      } else {
        out.room(4);
        let n;
        if (type === "f") {
          n = Math.round(value * 256) | 0;
          out.view.setInt32(out.length, n, true);
        } else if (type === "i") {
          out.view.setInt32(out.length, value | 0, true);
        } else {
          n = value === null || value === undefined ? 0 : typeof value === "object" ? value.id : value;
          out.view.setUint32(out.length, n >>> 0, true);
        }
        out.length += 4;
      }
    }
    const size = out.length - start;
    out.view.setUint32(start, object.id >>> 0, true);
    out.view.setUint32(start + 4, (size << 16) | opcode, true);
    if (ws.debug) {
      log("  -> " + object.interface + "@" + object.id + "." + name + "(" + values.map(shown).join(", ") + ")");
    }
    if (out.length > 60000) {
      ws.flush(client);
    }
  };

  const shown = (v) => typeof v === "string" ? JSON.stringify(v) : v && typeof v === "object" ?
    (v.interface ? v.interface + "@" + v.id : "[" + (v.length || v.byteLength) + " bytes]") : String(v);

  // Write what is waiting for a client to its socket. What the socket does not take now stays, and the
  // client is watched for room (compositor.js: writable). Returns false if the client is gone.
  ws.flush = (client) => {
    const out = client.out;
    if (client.gone || out.length === 0) {
      return !client.gone;
    }
    const m = u8();
    const v = dv();
    let sent = 0;
    while (sent < out.length) {
      const part = Math.min(out.length - sent, DATA_SIZE);
      m.set(out.bytes.subarray(sent, sent + part), DATA);
      v.setBigUint64(IOV, BigInt(DATA), true);
      v.setBigUint64(IOV + 8, BigInt(part), true);
      m.fill(0, MSG, MSG + 56);
      v.setBigUint64(MSG + 16, BigInt(IOV), true);
      v.setBigUint64(MSG + 24, 1n, true);
      // Descriptors go with the first bytes (the message they belong to is in them, or before them).
      const fds = sent === 0 ? out.fds.splice(0, MAX_FDS) : [];
      if (fds.length) {
        v.setBigUint64(CTL, BigInt(16 + 4 * fds.length), true);
        v.setInt32(CTL + 8, SOL_SOCKET, true);
        v.setInt32(CTL + 12, SCM_RIGHTS, true);
        fds.forEach((fd, i) => v.setInt32(CTL + 16 + 4 * i, fd, true));
        v.setBigUint64(MSG + 32, BigInt(CTL), true);
        v.setBigUint64(MSG + 40, BigInt((16 + 4 * fds.length + 7) & ~7), true);
      }
      const n = sys3(SYS.sendmsg, client.fd, MSG, MSG_DONTWAIT | MSG_NOSIGNAL);
      if (n === -EINTR) {
        out.fds.unshift(...fds);
        continue;
      }
      if (n === -EAGAIN) {
        out.fds.unshift(...fds);
        break;
      }
      if (n < 0) {
        ws.disconnect(client, "write error " + -n);
        return false;
      }
      for (const fd of fds) {
        if (closeLater.delete(fd)) {
          sys1(SYS.close, fd);	// (passed on: the client has its own copy now)
        }
      }
      sent += n;
      if (n < part) {
        break;	// (a short write: the rest waits for room)
      }
    }
    if (sent > 0 && sent < out.length) {
      out.bytes.copyWithin(0, sent, out.length);
    }
    out.length -= sent;
    client.wantWrite(out.length > 0);
    return true;
  };

  // Read what a client's socket has, decode the complete messages in it, and hand each to
  // ws.dispatch(client, object, request, args). Returns false if the client is gone.
  ws.receive = (client) => {
    const m = u8();
    const v = dv();
    for (;;) {
      m.fill(0, MSG, MSG + 56);
      v.setBigUint64(IOV, BigInt(DATA), true);
      v.setBigUint64(IOV + 8, BigInt(DATA_SIZE), true);
      v.setBigUint64(MSG + 16, BigInt(IOV), true);
      v.setBigUint64(MSG + 24, 1n, true);
      v.setBigUint64(MSG + 32, BigInt(CTL), true);
      v.setBigUint64(MSG + 40, BigInt(CTL_SIZE), true);
      const n = sys3(SYS.recvmsg, client.fd, MSG, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
      if (n === -EINTR) {
        continue;
      }
      if (n === -EAGAIN) {
        return true;
      }
      if (n <= 0) {
        ws.disconnect(client, n === 0 ? "closed" : "read error " + -n);
        return false;
      }
      // Descriptors that came with these bytes.
      const controllen = Number(v.getBigUint64(MSG + 40, true));
      for (let at = CTL; at + 16 <= CTL + controllen;) {
        const len = Number(v.getBigUint64(at, true));
        if (len < 16) {
          break;
        }
        if (v.getInt32(at + 8, true) === SOL_SOCKET && v.getInt32(at + 12, true) === SCM_RIGHTS) {
          for (let i = at + 16; i + 4 <= at + len; i += 4) {
            client.fds.push(v.getInt32(i, true));
          }
        }
        at += (len + 7) & ~7;
      }
      const flags = v.getUint32(MSG + 48, true);
      if (flags & 8) {	// MSG_CTRUNC: more descriptors than there was room for
        log("wire: client " + client.fd + " sent more descriptors at once than fit (" + MAX_FDS + ")");
      }
      // Append to what was left over, and decode whole messages.
      if (client.inLength + n > client.inBytes.length) {
        const bigger = new Uint8Array(Math.max(client.inBytes.length * 2, client.inLength + n));
        bigger.set(client.inBytes.subarray(0, client.inLength));
        client.inBytes = bigger;
        client.inView = new DataView(bigger.buffer);
      }
      client.inBytes.set(m.subarray(DATA, DATA + n), client.inLength);
      client.inLength += n;
      if (!decode(client)) {
        return false;
      }
      if (n < DATA_SIZE) {
        return true;
      }
    }
  };

  const decode = (client) => {
    const b = client.inBytes;
    const v = client.inView;
    let at = 0;
    while (client.inLength - at >= 8) {
      const id = v.getUint32(at, true);
      const word = v.getUint32(at + 4, true);
      const size = word >>> 16, opcode = word & 0xffff;
      if (size < 8 || size & 3) {
        ws.disconnect(client, "malformed message (size " + size + ")");
        return false;
      }
      if (client.inLength - at < size) {
        break;
      }
      const object = client.objects.get(id);
      if (!object) {
        // (Requests to an object the client has destroyed may still be in flight: not an error.)
        if (ws.debug) {
          log("  <- @" + id + " (unknown object) opcode " + opcode);
        }
        at += size;
        continue;
      }
      const iface = protocol[object.interface];
      const request = iface && iface.requests[opcode];
      if (!request) {
        ws.error(client, object, 1, "invalid method " + opcode + " of " + object.interface);
        return false;
      }
      // The arguments.
      const args = [];
      let p = at + 8;
      const end = at + size;
      let bad = false;
      for (const arg of request.args) {
        const type = arg[1].replace("?", "");
        if (type === "h") {
          if (!client.fds.length) {
            bad = true;
            break;
          }
          args.push(client.fds.shift());
          continue;
        }
        if (p + 4 > end) {
          bad = true;
          break;
        }
        if (type === "s" || type === "a") {
          const len = v.getUint32(p, true);
          p += 4;
          const padded = (len + 3) & ~3;
          if (p + padded > end) {
            bad = true;
            break;
          }
          if (type === "s") {
            args.push(len === 0 ? null : decoder.decode(b.subarray(p, p + len - 1)));
          } else {
            args.push(b.slice(p, p + len));
          }
          p += padded;
        } else if (type === "n" && arg.length > 2 && arg[2] === null) {
          // A new id of an interface the client names (wl_registry.bind): name, version, id.
          const len = v.getUint32(p, true);
          p += 4;
          const padded = (len + 3) & ~3;
          if (p + padded + 8 > end) {
            bad = true;
            break;
          }
          const name = decoder.decode(b.subarray(p, p + len - 1));
          p += padded;
          const version = v.getUint32(p, true);
          const newId = v.getUint32(p + 4, true);
          p += 8;
          args.push({ interface: name, version, id: newId });
        } else if (type === "f") {
          args.push(v.getInt32(p, true) / 256);
          p += 4;
        } else if (type === "i") {
          args.push(v.getInt32(p, true));
          p += 4;
        } else if (type === "o") {
          const oid = v.getUint32(p, true);
          p += 4;
          if (oid === 0) {
            args.push(null);
          } else {
            const o = client.objects.get(oid);
            if (!o) {
              ws.error(client, object, 0, "unknown object " + oid + " in " + object.interface + "." + request.name);
              return false;
            }
            args.push(o);
          }
        } else {
          args.push(v.getUint32(p, true));
          p += 4;
        }
      }
      if (bad) {
        ws.error(client, object, 1, "malformed arguments for " + object.interface + "." + request.name);
        return false;
      }
      if (ws.debug) {
        log("  <- " + object.interface + "@" + id + "." + request.name + "(" + args.map(shown).join(", ") + ")");
      }
      at += size;
      if (!ws.dispatch(client, object, request, args)) {
        return false;
      }
    }
    if (at > 0) {
      b.copyWithin(0, at, client.inLength);
      client.inLength -= at;
    }
    return true;
  };

  // A protocol error: the client is told (wl_display.error) and let go.
  ws.error = (client, object, code, message) => {
    log("wire: client " + client.fd + ": " + message);
    const display = client.objects.get(1);
    if (display) {
      ws.send(display, "error", object ? object.id : 0, code, message);
      ws.flush(client);
    }
    ws.disconnect(client, "protocol error");
  };

  // Descriptors we only pass on (a pipe's end from one client to another): closed once they are sent.
  const closeLater = new Set();
  ws.closeAfterSend = (fd) => {
    closeLater.add(fd);
  };
  // A client gone with descriptors still waiting to be sent to it: ours to close.
  ws.dropOutgoing = (client) => {
    for (const fd of client.out.fds) {
      if (closeLater.delete(fd)) {
        sys1(SYS.close, fd);
      }
    }
    client.out.fds = [];
  };

  ws.Outgoing = Outgoing;
})
