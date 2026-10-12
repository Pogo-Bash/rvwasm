// Video decoding for guest players: the gpuserver side of rvwasm_drv_video.so (gpu/vdec/driver, a VA-API
// driver) and of the page's decoder (tools/www/vdec-worker.js, WebCodecs). docs/video-decode.md has the design.
//
// A connection whose first command is VDEC_HELLO is a video client. What it says goes into a ring of its own
// (the "vdec ring", in this process's memory, announced as `vdec` beside the GPU ring), which the page's
// decoder reads; what the page has done it writes into the client's status file, a memory file both the client
// and this process map (decoded, drawn, copied, chunk bytes taken), so the client waits by reading it and this
// process answers nothing but HELLO and ALLOC. Chunks and images are memory files made here (contiguous, so
// the page reaches them in the machine's memory), mapped by the client too.
//
// The vdec ring (version 1), at `vdec`: a 4 KiB header (words; "page" marks the page's):
//   0 magic "RVVD", 4 version, 8 pid, 12 heartbeat, 16 data_size, 20 data_off, 24 head, 28 doorbell,
//   64 tail (page), 68 page_state (page: 1 ready, 3 no WebCodecs), 72 page_beat (page), 76 caps_n (page),
//   80 page_flags (page: 1 frames can be drawn into GPU resources), 1024: caps_n x {codec, profile, max_w,
//   max_h, flags} (page)
// then the records, as in the GPU ring (gpu/PROTOCOL.md): {op, client, serial, bytes} + payload, 16-byte
// aligned, a PAD (op 0) filling what is left at the end.
//   1 OPEN [status_lo, status_hi]          2 CLOSE []
//   3 MEM [mem, addr_lo, addr_hi, size]    4 MEM_END [mem]
//   5 STREAM [stream, codec, profile, level, width, height, flags]          6 STREAM_END [stream]
//   7 SURFACE [slot, width, height, res_y, res_uv]                          11 SLOT_END [slot]
//   8 DECODE [stream, slot, seq, addr_lo, addr_hi, size, flags, end]
//   9 COPY [slot, id, seq, addr_lo, addr_hi, off_y, pitch_y, off_uv, pitch_uv, width, height, size]
//   10 EXPORT [slot]
// The status file (ST_SIZE bytes): 0 magic "RVVS", 4 beat (page), 8 state (here: 1 open, 2 the page is gone),
// 12 consumed (page), from 4096: 4096 slots of {decoded, ready, copied, copy_status, error, 3 spare}.
(function (gs) {
  "use strict";

  const { dv, u8, i32, log } = gs;
  const C = { HELLO: 0x10010, ALLOC: 0x10011, FREE: 0x10012, STREAM: 0x10013, STREAM_END: 0x10014, SURFACE: 0x10015,
    DECODE: 0x10016, COPY: 0x10017, EXPORT: 0x10018, SLOT_END: 0x10019 };
  const SIZES = { [C.HELLO]: 1, [C.ALLOC]: 1, [C.FREE]: 1, [C.STREAM]: 7, [C.STREAM_END]: 1, [C.SURFACE]: 5,
    [C.DECODE]: 8, [C.COPY]: 11, [C.EXPORT]: 1, [C.SLOT_END]: 1 };
  const OP = { PAD: 0, OPEN: 1, CLOSE: 2, MEM: 3, MEM_END: 4, STREAM: 5, STREAM_END: 6, SURFACE: 7, DECODE: 8, COPY: 9,
    EXPORT: 10, SLOT_END: 11 };
  const H = { MAGIC: 0, VERSION: 4, PID: 8, HEARTBEAT: 12, DATA_SIZE: 16, DATA_OFF: 20, HEAD: 24, DOORBELL: 28,
    TAIL: 64, PAGE_STATE: 68, PAGE_BEAT: 72, CAPS_N: 76, PAGE_FLAGS: 80, CAPS: 1024 };
  const HEADER = 4096, DATA_SIZE = 256 * 1024, MAX_CAPS = 32;
  const ST_SIZE = 4096 + 32 * 4096, ST_STATE = 8;

  const block = gs.alloc(HEADER + DATA_SIZE + 4096);
  const ring = (block + 4095) & ~4095;
  const data = ring + HEADER;
  u8().fill(0, ring, ring + HEADER + DATA_SIZE);
  {
    const v = dv();
    v.setUint32(ring + H.MAGIC, 0x44565652, true);
    v.setUint32(ring + H.VERSION, 1, true);
    v.setUint32(ring + H.PID, gs.pid, true);
    v.setUint32(ring + H.DATA_SIZE, DATA_SIZE, true);
    v.setUint32(ring + H.DATA_OFF, HEADER, true);
  }
  const word = (off) => Atomics.load(i32(), Math.floor((ring + off) / 4)) >>> 0;
  const setWord = (off, value) => Atomics.store(i32(), Math.floor((ring + off) / 4), value | 0);

  let head = 0, serial = 0, bellDue = false;
  const room = () => DATA_SIZE - ((head - word(H.TAIL)) >>> 0);
  // A record (false: no room now; the command is tried again on a later turn).
  const record = (op, client, list) => {
    const bytes = 4 * list.length, total = 16 + ((bytes + 15) & ~15);
    const at = head % DATA_SIZE, pad = at + total > DATA_SIZE ? DATA_SIZE - at : 0;
    if (room() < pad + total) {
      return false;
    }
    const v = dv();
    if (pad) {
      v.setUint32(data + at, OP.PAD, true);
      v.setUint32(data + at + 4, 0, true);
      v.setUint32(data + at + 8, 0, true);
      v.setUint32(data + at + 12, pad - 16, true);
      head = (head + pad) >>> 0;
    }
    const where = data + head % DATA_SIZE;
    serial = (serial + 1) >>> 0;
    v.setUint32(where, op, true);
    v.setUint32(where + 4, client, true);
    v.setUint32(where + 8, serial, true);
    v.setUint32(where + 12, bytes, true);
    list.forEach((w, i) => v.setUint32(where + 16 + 4 * i, w >>> 0, true));
    head = (head + total) >>> 0;
    setWord(H.HEAD, head);
    bellDue = true;
    return true;
  };
  const doorbell = () => {
    if (bellDue) {
      bellDue = false;
      Atomics.add(i32(), Math.floor((ring + H.DOORBELL) / 4), 1);
      Atomics.notify(i32(), Math.floor((ring + H.DOORBELL) / 4));
    }
  };
  const lo = (a) => a % 0x100000000, hi = (a) => Math.floor(a / 0x100000000);

  // Memory files let go of once the page is past the record that ended them: { head, backing }.
  const later = [];
  const laterTurn = () => {
    const tail = word(H.TAIL);
    while (later.length && (pageGone || ((tail - later[0].head) | 0) >= 0)) {
      gs.letGo(later.shift().backing);
    }
  };

  // ---- the page's state: ready once it wrote caps; gone when its beat stops for 3 s
  let pageSeen = false, pageGone = false, beat = -1, beatAt = performance.now();
  const pageReady = () => !pageGone && word(H.PAGE_STATE) === 1;
  const pageCheck = (t) => {
    const b = word(H.PAGE_BEAT);
    if (b !== beat) {
      beat = b;
      beatAt = t;
      if (!pageSeen && word(H.PAGE_STATE) === 1) {
        pageSeen = true;
        log("gpuserver: the page's video decoder is here (" + word(H.CAPS_N) + " codecs, drawing "
            + (word(H.PAGE_FLAGS) & 1 ? "into GPU resources" : "not possible: copies only") + ")");
      }
    } else if (pageSeen && !pageGone && t - beatAt > 3000) {
      pageGone = true;
      log("gpuserver: the page's video decoder has not moved for 3 s: gone");
      for (const c of clients.values()) {
        if (c.status) {
          dv().setUint32(c.status.address + ST_STATE, 2, true);
        }
      }
    }
  };

  // ---- clients: { client (the server's), id, status (backing), mems: Map(id -> backing) }
  const clients = new Map();	// server client -> vdec state
  let nextId = 1, nextMem = 1;

  const caps = () => {
    const v = dv(), n = Math.min(word(H.CAPS_N), MAX_CAPS), list = [];
    for (let i = 0; i < n; i++) {
      for (let k = 0; k < 5; k++) {
        list.push(v.getUint32(ring + H.CAPS + 20 * i + 4 * k, true));
      }
    }
    // (the first entry carries whether drawing into GPU resources is possible)
    if (list.length && (word(H.PAGE_FLAGS) & 1)) {
      list[4] |= 4;
    }
    return list;
  };

  // A command of a video client: true when done, false to try again on a later turn, "gone".
  const command = (client, cmd, args) => {
    const v = dv();
    const a = (i) => v.getUint32(args + 4 * i, true);
    let s = clients.get(client);
    if (cmd === C.HELLO) {
      if (s) {
        return true;
      }
      if (pageGone || word(H.PAGE_STATE) === 3) {
        return gs.reply(client, C.HELLO, [pageGone ? 2 : 4, 0]) ? true : "gone";
      }
      if (!pageReady()) {
        return gs.reply(client, C.HELLO, [1, 0]) ? true : "gone";
      }
      const b = gs.backing(ST_SIZE);
      if (b.error) {
        log("gpuserver: no status file for " + client.name + " (" + b.what + ": error " + b.error + ")");
        return gs.reply(client, C.HELLO, [3, 0]) ? true : "gone";
      }
      s = { id: nextId++, status: b, mems: new Map(), streams: new Set() };
      u8().fill(0, b.address, b.address + ST_SIZE);
      v.setUint32(b.address, 0x53565652, true);
      v.setUint32(b.address + ST_STATE, 1, true);
      if (!record(OP.OPEN, s.id, [lo(b.address), hi(b.address)])) {
        gs.letGo(b);
        return false;
      }
      clients.set(client, s);
      client.video = true;
      const list = caps();
      if (!gs.reply(client, C.HELLO, [0, list.length / 5, ...list])) {
        return "gone";
      }
      const ok = gs.sendFd(client, b.fd);
      gs.sent(b);
      log("gpuserver: video client " + s.id + " (" + client.name + ")");
      return ok ? true : "gone";
    }
    if (!s) {
      gs.disconnect(client, "a video command before VDEC_HELLO");
      return "gone";
    }
    switch (cmd) {
    case C.ALLOC: {
      const size = a(0);
      if (!size || size > 256 * 1024 * 1024) {
        return gs.reply(client, C.ALLOC, [0, 1]) ? true : "gone";
      }
      const b = gs.backing(size);
      if (b.error) {
        log("gpuserver: no memory file of " + size + " bytes for " + client.name + " (" + b.what + ": error " + b.error + ")");
        return gs.reply(client, C.ALLOC, [0, 1]) ? true : "gone";
      }
      const mem = nextMem++;
      if (!record(OP.MEM, s.id, [mem, lo(b.address), hi(b.address), size])) {
        gs.letGo(b);
        nextMem--;
        return false;
      }
      s.mems.set(mem, b);
      if (!gs.reply(client, C.ALLOC, [mem, 0])) {
        return "gone";
      }
      const ok = gs.sendFd(client, b.fd);
      gs.sent(b);
      return ok ? true : "gone";
    }
    case C.FREE: {
      const b = s.mems.get(a(0));
      if (!b) {
        return true;
      }
      if (!record(OP.MEM_END, s.id, [a(0)])) {
        return false;
      }
      s.mems.delete(a(0));
      later.push({ head, backing: b });
      return true;
    }
    case C.STREAM:
      s.streams.add(a(0));
      return record(OP.STREAM, s.id, [a(0), a(1), a(2), a(3), a(4), a(5), a(6)]);
    case C.STREAM_END:
      s.streams.delete(a(0));
      return record(OP.STREAM_END, s.id, [a(0)]);
    case C.SURFACE:
      return record(OP.SURFACE, s.id, [a(0), a(1), a(2), a(3), a(4)]);
    case C.SLOT_END:
      return record(OP.SLOT_END, s.id, [a(0)]);
    case C.EXPORT:
      return record(OP.EXPORT, s.id, [a(0)]);
    case C.DECODE: {
      // stream slot seq mem offset size flags end
      const b = s.mems.get(a(3));
      if (!b || a(4) + a(5) > b.size) {
        log("gpuserver: " + client.name + ": a chunk outside its memory file");
        gs.disconnect(client, "bad chunk");
        return "gone";
      }
      const at = b.address + a(4);
      return record(OP.DECODE, s.id, [a(0), a(1), a(2), lo(at), hi(at), a(5), a(6), a(7)]);
    }
    case C.COPY: {
      // slot id seq mem off_y pitch_y off_uv pitch_uv width height size
      const b = s.mems.get(a(3));
      if (!b || a(10) > b.size) {
        log("gpuserver: " + client.name + ": a copy into a memory file that is not there");
        gs.disconnect(client, "bad copy");
        return "gone";
      }
      return record(OP.COPY, s.id, [a(0), a(1), a(2), lo(b.address), hi(b.address), a(4), a(5), a(6), a(7), a(8), a(9), a(10)]);
    }
    }
    return true;
  };

  // The client went away: the page lets go of its streams and frames; its files go once the page is past it.
  const gone = (client) => {
    const s = clients.get(client);
    if (!s) {
      return;
    }
    clients.delete(client);
    while (!record(OP.CLOSE, s.id, []) && !pageGone) {
      doorbell();
      Atomics.wait(i32(), Math.floor((ring + H.TAIL) / 4), word(H.TAIL), 5);
      if (performance.now() - beatAt > 3000) {
        break;
      }
    }
    for (const b of s.mems.values()) {
      later.push({ head, backing: b });
    }
    later.push({ head, backing: s.status });
    doorbell();
  };

  const turn = (t) => {
    doorbell();
    laterTurn();
    pageCheck(t);
    setWord(H.HEARTBEAT, word(H.HEARTBEAT) + 1);
  };

  log("gpuserver: video decoding ring at " + ring);
  return { ring, SIZES, command, gone, turn, isVideo: (cmd) => SIZES[cmd] !== undefined, clients };
})
