// The vtest server's side, the ring to the page's executor, and the turns.
//
// What a client sends is vtest (virglrenderer's vtest/vtest_protocol.h, as
// Mesa's virgl_vtest_socket.c speaks it): a header of two little-endian
// dwords {length, command} and then the command's arguments. The length is
// not to be trusted for the size: virglrenderer's own server reads what each
// command has (TRANSFER_PUT2 says it is followed by its data and is not,
// CREATE_RENDERER's length is in bytes), so this one does the same (SIZES).
//
// What needs the GPU goes into the ring (gpu/PROTOCOL.md: the ring): one
// record per command, read by the executor in the page out of this
// process's memory. What a client waits for (caps, a busy wait) is answered
// from words the executor writes. A resource's backing is a file in memory
// made here, mapped here and by the client: on this kernel (no MMU) the
// address this process gets is the pages' place in the machine's memory,
// which is what the executor is told, so a transfer reads and writes the
// very bytes the client sees.
//
// Venus (Vulkan: Mesa's libvulkan_virtio with VN_DEBUG=vtest) speaks the same socket with its own commands
// (GET_PARAM, GET_CAPSET, CONTEXT_INIT, RESOURCE_CREATE_BLOB, SYNC_*, SUBMIT_CMD2); they go into the ring as
// the v3 records (gpu/webgpu/INTERFACES.md 7.1) for the page's vkr. Its timeline syncs are u64 words in this
// process's memory that the executor writes when the work signalling them is done: SYNC_READ, SYNC_WAIT and
// SYNC_WRITE are answered here from the words, never through the ring.
(function (gs) {
  "use strict";

  const { sys0, sys1, sys2, sys3, sys4, sys6, SYS, alloc, dv, u8, i32, path, P0, S, log, env, writeMem } = gs;
  const AF_UNIX = 1, SOCK_STREAM = 1, SOCK_NONBLOCK = 0o4000, SOCK_CLOEXEC = 0o2000000;
  const O_RDWR = 2, O_CREAT = 0o100, O_EXCL = 0o200, O_CLOEXEC = 0o2000000;
  const AT_FDCWD = -100;
  const PROT_READ = 1, PROT_WRITE = 2, MAP_SHARED = 1;
  const EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLLIN = 1, EPOLLERR = 8, EPOLLHUP = 16;
  const EINTR = 4, EAGAIN = 11;
  const MSG_DONTWAIT = 0x40, MSG_NOSIGNAL = 0x4000, MSG_CMSG_CLOEXEC = 0x40000000;
  const SOL_SOCKET = 1, SCM_RIGHTS = 1;
  const MAX_EVENTS = 32;
  const TURN_MS = 8;

  // ---- vtest
  const V = {
    GET_CAPS: 1, RESOURCE_CREATE: 2, RESOURCE_UNREF: 3, TRANSFER_GET: 4, TRANSFER_PUT: 5, SUBMIT_CMD: 6,
    RESOURCE_BUSY_WAIT: 7, CREATE_RENDERER: 8, GET_CAPS2: 9, PING_PROTOCOL_VERSION: 10, PROTOCOL_VERSION: 11,
    RESOURCE_CREATE2: 12, TRANSFER_GET2: 13, TRANSFER_PUT2: 14, GET_PARAM: 15, GET_CAPSET: 16,
    CONTEXT_INIT: 17, RESOURCE_CREATE_BLOB: 18, SYNC_CREATE: 19, SYNC_UNREF: 20, SYNC_READ: 21, SYNC_WRITE: 22,
    SYNC_WAIT: 23, SUBMIT_CMD2: 24,
    // Ours, not virglrenderer's (rv's emulated render node, rv/virtgpu.c; said in the ready file's
    // "features"): an eventfd that is written once the client's work so far is done (a fence as a file).
    RV_FENCE: 0x10001,
    // A buffer another connection made (a dmabuf passed between processes, or between two opens of the render
    // node in one, as libva's and Mesa's are): found by its backing file's identity in the export table and
    // attached to this connection's context too (rv/virtgpu.c, PRIME_FD_TO_HANDLE).
    RV_RESOURCE_IMPORT: 0x10002,
  };
  const NAMES = Object.fromEntries(Object.entries(V).map(([k, v]) => [v, k]));
  // Dwords of arguments each command has, when it is not the header's length (-1: the length, in dwords;
  // -2: the length, in bytes).
  const SIZES = {
    [V.GET_CAPS]: 0, [V.GET_CAPS2]: 0, [V.PING_PROTOCOL_VERSION]: 0, [V.PROTOCOL_VERSION]: 1,
    [V.CREATE_RENDERER]: -2, [V.RESOURCE_CREATE2]: 11, [V.RESOURCE_UNREF]: 1, [V.TRANSFER_GET2]: 10,
    [V.TRANSFER_PUT2]: 10, [V.SUBMIT_CMD]: -1, [V.RESOURCE_BUSY_WAIT]: 2, [V.RESOURCE_CREATE_BLOB]: 6,
    [V.GET_PARAM]: 1, [V.GET_CAPSET]: 2, [V.CONTEXT_INIT]: 1, [V.SYNC_CREATE]: 2, [V.SYNC_UNREF]: 1,
    [V.SYNC_READ]: 1, [V.SYNC_WRITE]: 3, [V.SYNC_WAIT]: -1, [V.SUBMIT_CMD2]: -1, [V.RV_FENCE]: 0,
    [V.RV_RESOURCE_IMPORT]: 3,
  };
  const PROTOCOL = 3;		// resource ids made here, backing for everything that has a size
  const BUSY_WAIT_FLAG_WAIT = 1;
  const CAPS_V1_SIZE = 308;	// sizeof(struct virgl_caps_v1) (virgl_hw.h)
  // venus
  const CAPSET_VENUS = 4;
  const PARAM_MAX_TIMELINE_COUNT = 1;
  const MAX_TIMELINES = 64;	// vkr's sync_queues (ring_idx 1..63; 0 is the CPU's)
  const SYNC_WAIT_FLAG_ANY = 1;
  const SUBMIT_CMD2_FLAG_RING_IDX = 1;
  const MAX_SYNCS = 4096;	// sync words (8 bytes each) in the block after the export table
  const VCAPS_AT = 2048;	// the venus capset in the caps area (vcaps_len: header word 88)

  // ---- the ring (PROTOCOL.md: the ring)
  const R = {
    MAGIC: 0, VERSION: 4, PID: 8, HEARTBEAT: 12, DATA_SIZE: 16, DATA_OFF: 20, HEAD: 24, DOORBELL: 28,
    SUBMITTED: 32, EXPORTS_LO: 40, EXPORTS_HI: 44, EXPORTS_CAP: 48,
    TAIL: 64, DONE: 68, EXEC_HEARTBEAT: 72, EXEC_STATE: 76, EXEC_ERROR: 80, CAPS_LEN: 84, VCAPS_LEN: 88,
    CTX_DONE: 1024,	// exec: per context, the newest serial of that context's records whose work is done
  };
  const MAX_CTX = 255;	// contexts 1..255 at once (their done words fill the header's second kilobyte)
  // (RES_ATTACH, RES_DETACH: a resource imported by another context, and let go of by it while others hold it:
  // virgl_renderer_ctx_attach_resource / detach in the executor)
  const OP = { PAD: 0, CTX_CREATE: 1, CTX_DESTROY: 2, RES_CREATE: 3, RES_UNREF: 4, SUBMIT: 5, TRANSFER_PUT: 6,
    TRANSFER_GET: 7, BLOB_CREATE: 8, VN_CTX_INIT: 9, VN_SUBMIT: 10, VN_BLOB_CREATE: 11, RES_ATTACH: 12, RES_DETACH: 13 };
  const HEADER = 2048;
  const DATA_SIZE = Number(env("RVGPU_RING_MIB") || 4) * 1024 * 1024;
  const MAX_SUBMIT = DATA_SIZE / 4;
  const CAPS_SIZE = 4096;
  const EXPORTS_CAP = 1024;	// backing files at once (EXPORT_SIZE bytes each; a 16-byte head before them)
  const EXPORT_SIZE = 32;	// dev, ino_lo, ino_hi, res, ctx, 3 spare

  const block = alloc(HEADER + DATA_SIZE + CAPS_SIZE + 16 + EXPORT_SIZE * EXPORTS_CAP + 8 * MAX_SYNCS + 4096);
  const ring = (block + 4095) & ~4095;
  const data = ring + HEADER;
  const caps = data + DATA_SIZE;
  const exportsAt = caps + CAPS_SIZE;	// generation (4), cap (4), 8 spare, then {dev, ino_lo, ino_hi, res}
  const syncsAt = exportsAt + 16 + EXPORT_SIZE * EXPORTS_CAP;	// u64 words, 8-aligned
  u8().fill(0, ring, syncsAt + 8 * MAX_SYNCS);
  const pid = sys0(SYS.getpid);
  {
    const v = dv();
    v.setUint32(ring + R.MAGIC, 0x50475652, true);
    v.setUint32(ring + R.VERSION, 2, true);
    v.setUint32(ring + R.PID, pid, true);
    v.setUint32(ring + R.DATA_SIZE, DATA_SIZE, true);
    v.setUint32(ring + R.DATA_OFF, HEADER, true);
    v.setUint32(ring + R.EXPORTS_LO, exportsAt % 0x100000000, true);
    v.setUint32(ring + R.EXPORTS_HI, Math.floor(exportsAt / 0x100000000), true);
    v.setUint32(ring + R.EXPORTS_CAP, EXPORTS_CAP, true);
    v.setUint32(exportsAt + 4, EXPORTS_CAP, true);
  }
  const word = (offset) => Atomics.load(i32(), (ring + offset) >> 2) >>> 0;
  const setWord = (offset, value) => Atomics.store(i32(), (ring + offset) >> 2, value | 0);

  let head = 0;		// bytes written (mod 2^32)
  let serial = 0;	// of the last record
  let doorbellDue = false;
  const stats = { records: 0, bytes: 0, submits: 0, waits: 0, waitMs: 0, syncWaits: 0 };

  const room = () => DATA_SIZE - ((head - word(R.TAIL)) >>> 0);
  // A record of `bytes` of payload; `fill(address)` writes the payload. False if the ring has no room now.
  const record = (op, ctx, bytes, fill, needsCompletion) => {
    const total = 16 + ((bytes + 15) & ~15);
    const at = head % DATA_SIZE;
    const pad = at + total > DATA_SIZE ? DATA_SIZE - at : 0;
    if (room() < pad + total) {
      return 0;
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
    v.setUint32(where + 4, ctx, true);
    v.setUint32(where + 8, serial, true);
    v.setUint32(where + 12, bytes, true);
    fill(where + 16);
    head = (head + total) >>> 0;
    if (needsCompletion) {
      setWord(R.SUBMITTED, serial);
    }
    setWord(R.HEAD, head);
    doorbellDue = true;
    stats.records++;
    stats.bytes += total;
    return serial;
  };
  const ringWords = (list) => (address) => {
    const v = dv();
    list.forEach((w, i) => v.setUint32(address + 4 * i, w >>> 0, true));
  };
  const ring32 = (op, ctx, list, needsCompletion) => record(op, ctx, 4 * list.length, ringWords(list), needsCompletion);
  const doorbell = () => {
    if (doorbellDue) {
      doorbellDue = false;
      Atomics.add(i32(), (ring + R.DOORBELL) >> 2, 1);
      Atomics.notify(i32(), (ring + R.DOORBELL) >> 2);
    }
  };
  // Serial comparison that survives wrapping.
  const reached = (done, target) => ((done - target) | 0) >= 0;

  // ---- the executor's state. While one answers, a file next to the socket says so: `desktop wayland`
  // points programs here only then (tools/x/desktop, rvgpu_use).
  const readyPath = () => socketPath + ".ready";
  // What it says is where the ring and the export table are (JSON), for the window server, which must not
  // depend on having heard an announcement in time (gpu/PROTOCOL.md: the ring).
  const setReady = (yes) => {
    if (yes) {
      const fd = sys4(SYS.openat, AT_FDCWD, path(readyPath(), P0), O_RDWR | O_CREAT | O_CLOEXEC | 0o1000, 0o644);
      if (fd >= 0) {
        gs.writeAll(fd, JSON.stringify({ pid, ring, exports: exportsAt, exportsCap: EXPORTS_CAP, features: ["fence", "import", "vdec"],
          vdec: vdec.ring }) + "\n");
        sys1(SYS.close, fd);
      }
    } else {
      sys3(SYS.unlinkat, AT_FDCWD, path(readyPath(), P0), 0);
    }
  };
  // And when none has answered within 10 s of our start, another file says that (rvgpu-0.none), so that
  // programs started later draw in software at once instead of waiting (an executor that comes later still
  // takes it away and writes the ready file).
  const nonePath = () => socketPath + ".none";
  const NONE_MS = 10000;
  let noneSaid = false;
  const setNone = (yes) => {
    noneSaid = yes;
    if (yes) {
      const fd = sys4(SYS.openat, AT_FDCWD, path(nonePath(), P0), O_RDWR | O_CREAT | O_CLOEXEC | 0o1000, 0o644);
      if (fd >= 0) {
        sys1(SYS.close, fd);
      }
    } else {
      sys3(SYS.unlinkat, AT_FDCWD, path(nonePath(), P0), 0);
    }
  };
  const startedAt = performance.now();
  let execSeen = 0, execBeat = -1, execBeatAt = performance.now(), execGone = false;
  const execReady = () => !execGone && word(R.EXEC_STATE) === 1 && word(R.CAPS_LEN) > 0;
  // (venus: an executor with vkr, which wrote the venus capset; it may have no virgl caps)
  const venusReady = () => !execGone && word(R.EXEC_STATE) === 1 && word(R.VCAPS_LEN) > 0;
  const execCheck = (t) => {
    const state = word(R.EXEC_STATE);
    if (state === 2 || state === 3) {
      if (!execGone) {
        log("gpuserver: the executor says " + (state === 2 ? "its context is lost" : "it failed")
            + " (error " + word(R.EXEC_ERROR) + "): no more GPU here");
        setReady(false);
      }
      execGone = true;
      return;
    }
    const beat = word(R.EXEC_HEARTBEAT);
    if (beat !== execBeat) {
      execBeat = beat;
      execBeatAt = t;
      if (state === 1 && !execSeen) {
        execSeen = 1;
        log("gpuserver: the executor is here (" + word(R.CAPS_LEN) + " bytes of caps, " + word(R.VCAPS_LEN)
            + " of the venus capset, " + Math.round(t - startedAt) + " ms after our start)");
        setReady(true);
        if (noneSaid) {
          setNone(false);
        }
      }
    } else if (!execSeen && !noneSaid && t - startedAt > NONE_MS) {
      log("gpuserver: no executor has answered in " + NONE_MS / 1000 + " s (a page without one?): programs draw in software");
      setNone(true);
    } else if (execSeen && !execGone && t - execBeatAt > 2000) {
      log("gpuserver: the executor has not moved for 2 s: gone");
      execGone = true;
      setReady(false);
    }
  };

  // ---- the export table: which backing file is which resource (for wlserver: a dmabuf's inode)
  const exportSlots = new Array(EXPORTS_CAP).fill(0);
  const exportAdd = (res, ctx, dev, ino) => {
    const i = exportSlots.indexOf(0);
    if (i < 0) {
      return -1;
    }
    exportSlots[i] = res;
    const v = dv();
    const at = exportsAt + 16 + EXPORT_SIZE * i;
    v.setUint32(at, dev >>> 0, true);
    v.setUint32(at + 4, ino % 0x100000000, true);
    v.setUint32(at + 8, Math.floor(ino / 0x100000000), true);
    v.setUint32(at + 16, ctx, true);
    v.setUint32(at + 12, res, true);	// (last: a reader takes an entry whose res is not 0)
    Atomics.add(i32(), exportsAt >> 2, 1);
    return i;
  };
  const exportRemove = (i) => {
    if (i >= 0) {
      exportSlots[i] = 0;
      dv().setUint32(exportsAt + 16 + EXPORT_SIZE * i + 12, 0, true);
      Atomics.add(i32(), exportsAt >> 2, 1);
    }
  };

  // ---- resources
  const dir = env("XDG_RUNTIME_DIR") || "/tmp";
  const resources = new Map();	// res -> { res, ctx, size, address, exportSlot }
  const unmaps = [];		// { serial, address, size }: backing to let go of once the executor is past serial
  let nextRes = 1, nextFile = 0;

  // Small backing files are kept for the next resource of the same size instead of being let go of:
  // Mesa's vtest winsys makes every fence a resource of 8 bytes (virgl_vtest_fence_create), one or more
  // a frame, and a file each time is six system calls and a descriptor passed. Kept ones are zeroed
  // before they are handed out, as a new file would be.
  // Descriptors: none is held for a resource after its descriptor went to the client. A live resource
  // costs no descriptor here (Mesa's resource cache keeps hundreds of small ones alive, and glmark2
  // --off-screen ran this server out of descriptors: "no backing of 8 bytes ... error 24"). So a file
  // that may be kept stays linked under its name while it exists, and is opened again when it is
  // handed out again; the others are unlinked at once.
  const KEEP_MAX = 65536, KEEP_COUNT = 64;
  const kept = new Map();	// size -> [backing]
  let keptCount = 0;
  const keep = (b) => {
    if (!b.name || b.size > KEEP_MAX || keptCount >= KEEP_COUNT) {
      return false;
    }
    if (!kept.has(b.size)) {
      kept.set(b.size, []);
    }
    kept.get(b.size).push(b);
    keptCount++;
    return true;
  };
  // A backing file of `size` bytes, mapped: { fd, address, size, dev, ino, name } or { error }. Its
  // descriptor is the caller's to close (sent) once it went to the client.
  const backing = (size) => {
    const list = kept.get(size);
    while (list && list.length) {
      const b = list.pop();
      keptCount--;
      const fd = sys4(SYS.openat, AT_FDCWD, path(b.name, P0), O_RDWR | O_CLOEXEC, 0);
      if (fd < 0) {	// (gone from under us: let it go and make a new one)
        sys2(SYS.munmap, b.address, b.size);
        continue;
      }
      b.fd = fd;
      u8().fill(0, b.address, b.address + size);
      return b;
    }
    const name = dir + "/.rvgpu-" + pid + "-" + nextFile++;
    const fd = sys4(SYS.openat, AT_FDCWD, path(name, P0), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0o600);
    if (fd < 0) {
      return { error: -fd, what: "open " + name };
    }
    const named = size <= KEEP_MAX;
    if (!named) {
      sys3(SYS.unlinkat, AT_FDCWD, path(name, P0), 0);
    }
    const fail = (r, what) => {
      sys1(SYS.close, fd);
      if (named) {
        sys3(SYS.unlinkat, AT_FDCWD, path(name, P0), 0);
      }
      return { error: -r, what };
    };
    // Up from size 0 once: the only way a ramfs file gets pages in one piece that can be mapped shared.
    let r = sys2(SYS.ftruncate, fd, size);
    if (r < 0) {
      return fail(r, "ftruncate");
    }
    r = sys6(SYS.mmap, 0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r < 0 && r > -4096) {
      return fail(r, "mmap " + size);
    }
    const address = r;
    sys2(SYS.fstat, fd, S);
    const v = dv();
    const dev = Number(v.getBigUint64(S, true) & 0xffffffffn);
    const ino = Number(v.getBigUint64(S + 8, true));
    return { fd, address, size, dev, ino, name: named ? name : null };
  };
  // After the descriptor went to the client (or could not): closed.
  const sent = (b) => {
    if (b.fd >= 0) {
      sys1(SYS.close, b.fd);
      b.fd = -1;
    }
  };
  const letGo = (b) => {
    sent(b);
    if (!keep(b)) {
      sys2(SYS.munmap, b.address, b.size);
      if (b.name) {
        sys3(SYS.unlinkat, AT_FDCWD, path(b.name, P0), 0);
      }
    }
  };
  // (at exit: the files still linked)
  const unlinkAll = () => {
    for (const r of resources.values()) {
      if (r.backing && r.backing.name) {
        sys3(SYS.unlinkat, AT_FDCWD, path(r.backing.name, P0), 0);
      }
    }
    for (const list of kept.values()) {
      for (const b of list) {
        sys3(SYS.unlinkat, AT_FDCWD, path(b.name, P0), 0);
      }
    }
    for (const u of unmaps) {
      if (u.backing.name) {
        sys3(SYS.unlinkat, AT_FDCWD, path(u.backing.name, P0), 0);
      }
    }
  };

  // A record that cannot be refused for room: if the ring is full, wait for it (the executor must hear of it).
  const ringSure = (op, ctx, list) => {
    let s;
    while (!(s = ring32(op, ctx, list, false))) {
      doorbell();
      Atomics.wait(i32(), (ring + R.TAIL) >> 2, word(R.TAIL), 5);
      if (execGone) {
        break;
      }
    }
    return s;
  };
  // `client` lets go of resource `res`. A resource lives while a connection holds it (the one that made it, and
  // those that imported it): the others only detach it from their contexts.
  const resourceGone = (res, client) => {
    const r = resources.get(res);
    if (!r || !r.holders.has(client)) {
      return;
    }
    r.holders.delete(client);
    client.resources.delete(res);
    if (r.holders.size) {
      if (client.created) {
        ringSure(OP.RES_DETACH, client.ctx, [res]);
      }
      if (r.ctx === client.ctx) {
        r.ctx = r.holders.values().next().value.ctx;
      }
      return;
    }
    resources.delete(res);
    exportRemove(r.exportSlot);
    // The record cannot be refused for room: if the ring is full, wait for it (the executor must hear of it).
    let s;
    while (!(s = ring32(OP.RES_UNREF, client.ctx, [res], false))) {
      doorbell();
      Atomics.wait(i32(), (ring + R.TAIL) >> 2, word(R.TAIL), 5);
      if (execGone) {
        break;
      }
    }
    if (r.backing) {
      unmaps.push({ serial: s || serial, backing: r.backing });
    }
  };

  // Fences as files (V.RV_FENCE): { client, ctx, target, fd }, written (and our copy closed) when the
  // context's done word passes the target, or the executor is gone, or the client went away.
  const fences = [];
  const fenceSignal = (f) => {
    const v = dv();
    v.setBigUint64(S + 60000, 1n, true);
    sys3(SYS.write, f.fd, S + 60000, 8);
    sys1(SYS.close, f.fd);
  };
  const fenceTurn = () => {
    for (let i = 0; i < fences.length;) {
      const f = fences[i];
      if (execGone || f.client.gone || reached(word(R.CTX_DONE + 4 * f.ctx), f.target)) {
        fenceSignal(f);
        fences.splice(i, 1);
      } else {
        i++;
      }
    }
  };

  const unmapTurn = () => {
    const done = word(R.DONE);
    while (unmaps.length && (execGone || reached(done, unmaps[0].serial))) {
      letGo(unmaps.shift().backing);
    }
  };

  // ---- venus syncs: u64 words at syncsAt + 8 * id (ids 1 .. MAX_SYNCS - 1). The executor writes a word
  // (low half, then high) when the batch that signals it is done; a word is used again only when no batch
  // still to be done will write it.
  const syncOwner = new Array(MAX_SYNCS).fill(null);
  const syncPending = new Array(MAX_SYNCS).fill(0n);	// the largest value a submitted batch will write
  const syncAddr = (id) => syncsAt + 8 * id;
  const syncGet = (id) => {
    const w = i32(), at = syncAddr(id) >> 2;
    for (;;) {
      const hi = Atomics.load(w, at + 1) >>> 0, lo = Atomics.load(w, at) >>> 0;
      if ((Atomics.load(w, at + 1) >>> 0) === hi) {
        return (BigInt(hi) << 32n) | BigInt(lo);
      }
    }
  };
  const syncSet = (id, v) => {
    const w = i32(), at = syncAddr(id) >> 2;
    Atomics.store(w, at, Number(v & 0xffffffffn) | 0);
    Atomics.store(w, at + 1, Number(v >> 32n) | 0);
  };
  let syncNext = 1;
  const syncNew = (client) => {
    for (let k = 0; k < MAX_SYNCS - 1; k++) {
      const id = 1 + (syncNext - 1 + k) % (MAX_SYNCS - 1);
      if (!syncOwner[id] && syncGet(id) >= syncPending[id]) {
        syncNext = id + 1;
        syncOwner[id] = client;
        syncPending[id] = 0n;
        return id;
      }
    }
    return 0;
  };
  const syncOf = (client, id) => (id > 0 && id < MAX_SYNCS && syncOwner[id] === client ? id : 0);
  // SYNC_WAIT: the client polls an eventfd that is written when the syncs reach their values
  const syncWaits = [];	// { client, fd, any, deadline, list: [{ id, value }] }
  const syncWaitTurn = () => {
    const t = performance.now();
    for (let i = 0; i < syncWaits.length;) {
      const w = syncWaits[i];
      let met = 0;
      for (const e of w.list) {
        met += e.id && syncGet(e.id) >= e.value ? 1 : 0;
      }
      const done = w.any ? met > 0 : met === w.list.length;
      if (done || w.client.gone || t > w.deadline) {
        if (done && !w.client.gone) {
          dv().setBigUint64(S, 1n, true);
          sys3(SYS.write, w.fd, S, 8);
        }
        sys1(SYS.close, w.fd);
        syncWaits.splice(i, 1);
      } else {
        i++;
      }
    }
  };

  // ---- the socket
  const socketPath = env("RVGPU_SOCKET") || dir + "/rvgpu-0";
  if (socketPath.length > 107) {
    log("gpuserver: the socket's path is too long: " + socketPath);
    gs.exit(1);
  }
  sys3(SYS.unlinkat, AT_FDCWD, path(socketPath, P0), 0);
  setReady(false);
  const listener = sys3(SYS.socket, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    log("gpuserver: socket: error " + -listener);
    gs.exit(1);
  }
  {
    const addr = alloc(112);
    u8().fill(0, addr, addr + 112);
    dv().setUint16(addr, AF_UNIX, true);
    const raw = new TextEncoder().encode(socketPath);
    u8().set(raw, addr + 2);
    let r = sys3(SYS.bind, listener, addr, 2 + raw.length + 1);
    if (r >= 0) {
      r = sys2(SYS.listen, listener, 64);
    }
    if (r < 0) {
      log("gpuserver: bind/listen " + socketPath + ": error " + -r);
      gs.exit(1);
    }
  }

  const epfd = sys1(SYS.epoll_create1, O_CLOEXEC);
  const events = alloc(16 * MAX_EVENTS);
  const one = alloc(16);
  const watch = (op, fd, mask) => {
    const v = dv();
    v.setUint32(one, mask, true);
    v.setUint32(one + 4, 0, true);
    v.setBigUint64(one + 8, BigInt(fd), true);
    return sys4(SYS.epoll_ctl, epfd, op, fd, one);
  };
  watch(EPOLL_CTL_ADD, listener, EPOLLIN);

  // A client's bytes are read into a block of this process's memory, so that a command buffer goes from
  // there into the ring with one copy within the memory.
  const IN_SIZE = MAX_SUBMIT + 65536;
  const clients = new Map();
  const freeCtx = [];	// context numbers to use again (their records all done)
  let nextCtx = 1;
  const newCtx = () => {
    if (freeCtx.length && (execGone || reached(word(R.DONE), freeCtx[0].serial))) {
      return freeCtx.shift().ctx;
    }
    return nextCtx <= MAX_CTX ? nextCtx++ : 0;
  };

  const accept = () => {
    for (;;) {
      const fd = sys4(SYS.accept4, listener, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd === -EINTR) {
        continue;
      }
      if (fd < 0) {
        return;
      }
      const ctx = execGone ? 0 : newCtx();
      if (!ctx) {
        log("gpuserver: " + (execGone ? "no executor" : MAX_CTX + " clients already") + ": connection refused");
        sys1(SYS.close, fd);
        continue;
      }
      setWord(R.CTX_DONE + 4 * ctx, 0);
      const client = {
        fd, ctx, name: "", nameBytes: null, venus: false, version: 0, created: false, gone: false,
        input: 0, length: 0, wait: null, last: 0, resources: new Set(), since: performance.now(),
      };
      clients.set(fd, client);
      watch(EPOLL_CTL_ADD, fd, EPOLLIN);
      if (gs.debug) {
        log("gpuserver: client " + fd + " connected (context " + client.ctx + ")");
      }
    }
  };

  const disconnect = (client, why) => {
    if (client.gone) {
      return;
    }
    client.gone = true;
    log("gpuserver: client " + client.fd + " (" + (client.name || "?") + ", context " + client.ctx + ") gone: " + why);
    watch(EPOLL_CTL_DEL, client.fd, 0);
    clients.delete(client.fd);
    sys1(SYS.close, client.fd);
    for (const res of [...client.resources]) {
      resourceGone(res, client);
    }
    if (client.video) {
      vdec.gone(client);
    }
    // (its batches will never be done: their syncs are free once let go of)
    for (let id = 1; id < MAX_SYNCS; id++) {
      if (syncOwner[id] === client) {
        syncOwner[id] = null;
        syncPending[id] = 0n;
      }
    }
    if (client.created) {
      while (!ring32(OP.CTX_DESTROY, client.ctx, [], false) && !execGone) {
        doorbell();
        Atomics.wait(i32(), (ring + R.TAIL) >> 2, word(R.TAIL), 5);
      }
    }
    freeCtx.push({ ctx: client.ctx, serial });
    if (client.input) {
      gs.free(client.input);
      client.input = 0;
    }
  };

  // Replies: a header and dwords, written whole (the client is blocked reading them).
  const reply = (client, cmd, list) => {
    const v = dv();
    v.setUint32(S, list.length, true);
    v.setUint32(S + 4, cmd, true);
    list.forEach((w, i) => v.setUint32(S + 8 + 4 * i, w >>> 0, true));
    if (writeMem(client.fd, S, 8 + 4 * list.length) < 0) {
      disconnect(client, "write error");
      return false;
    }
    return true;
  };
  // A descriptor, with the one byte Mesa's virgl_vtest_receive_fd() reads beside it.
  const sendFd = (client, fd) => {
    const v = dv();
    const MSG = S, IOV = S + 64, CTL = S + 96, BYTE = S + 128;
    u8().fill(0, MSG, MSG + 136);
    v.setBigUint64(IOV, BigInt(BYTE), true);
    v.setBigUint64(IOV + 8, 1n, true);
    v.setBigUint64(MSG + 16, BigInt(IOV), true);
    v.setBigUint64(MSG + 24, 1n, true);
    v.setBigUint64(CTL, 20n, true);
    v.setInt32(CTL + 8, SOL_SOCKET, true);
    v.setInt32(CTL + 12, SCM_RIGHTS, true);
    v.setInt32(CTL + 16, fd, true);
    v.setBigUint64(MSG + 32, BigInt(CTL), true);
    v.setBigUint64(MSG + 40, 24n, true);
    for (;;) {
      const n = sys3(SYS.sendmsg, client.fd, MSG, MSG_NOSIGNAL);
      if (n === -EINTR || n === -EAGAIN) {
        if (n === -EAGAIN) {
          v.setInt32(S + 200, client.fd, true);
          v.setInt16(S + 204, 4, true);
          v.setInt16(S + 206, 0, true);
          sys6(SYS.ppoll, S + 200, 1, 0, 0, 0, 0);
        }
        continue;
      }
      if (n < 0) {
        disconnect(client, "sendmsg error " + -n);
        return false;
      }
      return true;
    }
  };

  // ---- commands. Each returns true when done, false to be called again later (the client waits: for
  // the executor's caps, for room in the ring, for its work to be done), "gone" if the client went away.
  const capsReply = (client, cmd) => {
    const len = word(R.CAPS_LEN);
    const v = dv();
    if (cmd === V.GET_CAPS2) {
      // vtest_send_caps2: the length word is the caps' size in BYTES, plus one.
      v.setUint32(S, len + 1, true);
      v.setUint32(S + 4, 2, true);
      u8().copyWithin(S + 8, caps, caps + len);
      return writeMem(client.fd, S, 8 + len) >= 0;
    }
    v.setUint32(S, CAPS_V1_SIZE / 4 + 1, true);
    v.setUint32(S + 4, 1, true);
    u8().copyWithin(S + 8, caps, caps + CAPS_V1_SIZE);
    return writeMem(client.fd, S, 8 + CAPS_V1_SIZE) >= 0;
  };

  const command = (client, cmd, len, args) => {
    const v = dv();
    const arg = (i) => v.getUint32(args + 4 * i, true);
    switch (cmd) {
    case V.CREATE_RENDERER: {
      // The context is made with the first record that needs it: CTX_CREATE for virgl, VN_CTX_INIT for a
      // client that says CONTEXT_INIT (venus) first, so that the page's front ends make only what is used.
      client.name = gs.cstring(args).slice(0, 63) || "?";
      client.nameBytes = u8().slice(args, args + len);
      return true;
    }
    case V.GET_PARAM: {
      const valid = arg(0) === PARAM_MAX_TIMELINE_COUNT;
      return reply(client, V.GET_PARAM, [valid ? 1 : 0, valid ? MAX_TIMELINES : 0]);
    }
    case V.GET_CAPSET: {
      if (!venusReady()) {
        // (an executor without vkr says so with virgl caps alone: no venus here)
        if (execGone || execReady() || performance.now() - client.since > 10000) {
          return reply(client, V.GET_CAPSET, [0]);
        }
        return false;
      }
      const vlen = word(R.VCAPS_LEN);
      if (arg(0) !== CAPSET_VENUS || arg(1) > 0) {
        return reply(client, V.GET_CAPSET, [0]);
      }
      const v = dv();
      const list = [1];
      for (let i = 0; i < vlen; i += 4) {
        list.push(v.getUint32(caps + VCAPS_AT + i, true));
      }
      return reply(client, V.GET_CAPSET, list);
    }
    case V.CONTEXT_INIT: {
      if (arg(0) !== CAPSET_VENUS || client.created) {
        disconnect(client, "CONTEXT_INIT with capset " + arg(0) + (client.created ? " after its context was made" : ""));
        return "gone";
      }
      const name = client.nameBytes || new Uint8Array(1);
      if (!record(OP.VN_CTX_INIT, client.ctx, 4 + name.length + 1, (to) => {
        dv().setUint32(to, CAPSET_VENUS, true);
        u8().set(name, to + 4);
        u8()[to + 4 + name.length] = 0;
      }, false)) {
        return false;
      }
      client.created = true;
      client.venus = true;
      return true;
    }
    case V.SYNC_CREATE: {
      const id = syncNew(client);
      if (!id) {
        disconnect(client, MAX_SYNCS + " syncs already");
        return "gone";
      }
      syncSet(id, (BigInt(arg(1)) << 32n) | BigInt(arg(0)));
      return reply(client, V.SYNC_CREATE, [id]);
    }
    case V.SYNC_UNREF: {
      const id = syncOf(client, arg(0));
      if (id) {
        syncOwner[id] = null;
      }
      return true;
    }
    case V.SYNC_READ: {
      const id = syncOf(client, arg(0));
      const value = id ? syncGet(id) : 0n;
      return reply(client, V.SYNC_READ, [Number(value & 0xffffffffn), Number(value >> 32n)]);
    }
    case V.SYNC_WRITE: {
      const id = syncOf(client, arg(0));
      if (id) {
        syncSet(id, (BigInt(arg(2)) << 32n) | BigInt(arg(1)));
      }
      return true;
    }
    case V.SYNC_WAIT: {
      // flags timeout_ms, then {id, value_lo, value_hi} each
      const n = Math.floor((len - 2) / 3);
      const list = [];
      for (let i = 0; i < n; i++) {
        list.push({ id: syncOf(client, arg(2 + 3 * i)),
          value: (BigInt(arg(4 + 3 * i)) << 32n) | BigInt(arg(3 + 3 * i)) });
      }
      const fd = sys2(SYS.eventfd2, 0, O_CLOEXEC);
      if (fd < 0) {
        log("gpuserver: eventfd: error " + -fd);
        disconnect(client, "no eventfd");
        return "gone";
      }
      const timeout = arg(1);
      // (past the client's own timeout it has stopped polling; keep the fd a little longer, then let go)
      syncWaits.push({ client, fd, any: (arg(0) & SYNC_WAIT_FLAG_ANY) !== 0, list,
        deadline: timeout === 0xffffffff ? Infinity : performance.now() + timeout + 1000 });
      stats.syncWaits++;
      if (!reply(client, V.SYNC_WAIT, []) || !sendFd(client, fd)) {
        return "gone";
      }
      syncWaitTurn();
      return true;
    }
    case V.SUBMIT_CMD2: {
      if (!client.venus) {
        disconnect(client, "SUBMIT_CMD2 without a venus context");
        return "gone";
      }
      if (4 * len > MAX_SUBMIT) {
        disconnect(client, "a SUBMIT_CMD2 of " + 4 * len + " bytes (at most " + MAX_SUBMIT + ")");
        return "gone";
      }
      const s = submitCmd2(client, args, len);
      if (s === "bad") {
        disconnect(client, "a SUBMIT_CMD2 that does not add up");
        return "gone";
      }
      if (!s) {
        return false;
      }
      client.last = s;
      stats.submits++;
      return true;
    }
    case V.PING_PROTOCOL_VERSION:
      return reply(client, V.PING_PROTOCOL_VERSION, []);
    case V.PROTOCOL_VERSION: {
      let version = Math.min(arg(0), PROTOCOL);
      if (version === 1) {
        version = 0;
      }
      if (version < 3) {
        // (Mesa's virgl asks for 4 and drops to 2 only for a host whose caps are older than
        // virglrenderer's feature check 23; venus needs 3. Version 2's handles are the client's own.)
        log("gpuserver: " + client.name + " speaks vtest " + arg(0) + ": too old (3 or newer is needed)");
        disconnect(client, "protocol too old");
        return "gone";
      }
      client.version = version;
      return reply(client, V.PROTOCOL_VERSION, [version]);
    }
    case V.GET_CAPS:
    case V.GET_CAPS2:
      if (!execReady()) {
        if (execGone || performance.now() - client.since > 10000) {
          disconnect(client, "no executor to answer for caps");
          return "gone";
        }
        return false;
      }
      return capsReply(client, cmd);
    case V.RESOURCE_BUSY_WAIT: {
      const flags = arg(1);
      // Clients ask once, with handle 0, before anything else to learn whether PING is understood.
      const target = client.last;
      const done = word(R.CTX_DONE + 4 * client.ctx);
      const busy = target && !reached(done, target);
      if (busy && (flags & BUSY_WAIT_FLAG_WAIT)) {
        if (execGone) {
          disconnect(client, "the executor is gone");
          return "gone";
        }
        if (!client.wait) {
          client.wait = performance.now();
          stats.waits++;
        }
        return false;
      }
      if (client.wait) {
        stats.waitMs += performance.now() - client.wait;
        client.wait = null;
      }
      return reply(client, V.RESOURCE_BUSY_WAIT, [busy ? 1 : 0]);
    }
    case V.RESOURCE_CREATE2: {
      if (!ensureCtx(client)) {
        return false;
      }
      // target format bind width height depth array_size last_level nr_samples data_size (handle first: 0)
      const size = arg(10);
      let b = null;
      if (size) {
        b = backing(size);
        if (b.error) {
          log("gpuserver: no backing of " + size + " bytes for " + client.name + " (" + b.what + ": error " + b.error + ")");
          disconnect(client, "out of memory");
          return "gone";
        }
      }
      const res = nextRes++;
      const address = b ? b.address : 0;
      const list = [res, arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7), arg(8), arg(9), 0, size,
        address % 0x100000000, Math.floor(address / 0x100000000)];
      if (!ring32(OP.RES_CREATE, client.ctx, list, false)) {
        if (b) {
          letGo(b);
        }
        nextRes--;
        return false;
      }
      resources.set(res, { res, ctx: client.ctx, size, address, backing: b, holders: new Set([client]),
        info: [arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7), arg(8)],
        exportSlot: b ? exportAdd(res, client.ctx, b.dev, b.ino) : -1 });
      client.resources.add(res);
      if (!reply(client, V.RESOURCE_CREATE2, [res])) {
        return "gone";
      }
      if (b) {
        const ok = sendFd(client, b.fd);
        sent(b);
        if (!ok) {
          return "gone";
        }
      }
      return true;
    }
    case V.RESOURCE_CREATE_BLOB: {
      // type flags size_lo size_hi id_lo id_hi. virgl: the resource itself was made by a PIPE_RESOURCE_CREATE
      // with this blob id in a command buffer before. venus: blob id 0 is shared memory (rings, reply and
      // command streams), any other the VkDeviceMemory of that id, whose bytes these pages become.
      if (client.venus) {
        return venusBlob(client, arg);
      }
      if (!ensureCtx(client)) {
        return false;
      }
      const size = arg(2) + arg(3) * 0x100000000;
      const b = backing(size);
      if (b.error) {
        log("gpuserver: no blob of " + size + " bytes for " + client.name + " (" + b.what + ": error " + b.error + ")");
        disconnect(client, "out of memory");
        return "gone";
      }
      const res = nextRes++;
      const list = [res, arg(4), size, arg(1), b.address % 0x100000000, Math.floor(b.address / 0x100000000), arg(0)];
      if (!ring32(OP.BLOB_CREATE, client.ctx, list, false)) {
        letGo(b);
        nextRes--;
        return false;
      }
      resources.set(res, { res, ctx: client.ctx, size, address: b.address, backing: b, holders: new Set([client]),
        info: [0, 0, 0, size, 1, 1, 1, 0], exportSlot: exportAdd(res, client.ctx, b.dev, b.ino) });
      client.resources.add(res);
      if (!reply(client, V.RESOURCE_CREATE_BLOB, [res])) {
        return "gone";
      }
      const ok = sendFd(client, b.fd);
      sent(b);
      return ok ? true : "gone";
    }
    case V.RESOURCE_UNREF:
      resourceGone(arg(0), client);
      return true;
    case V.RV_RESOURCE_IMPORT: {
      // dev (low 32 bits), ino_lo, ino_hi -> res, target, format, bind, width, height, depth, array_size,
      // last_level, size (res 0: no such buffer here)
      const ino = arg(1) + arg(2) * 0x100000000;
      let found = null;
      for (const r of resources.values()) {
        if (r.backing && (r.backing.dev >>> 0) === arg(0) && r.backing.ino === ino) {
          found = r;
          break;
        }
      }
      if (!found) {
        return reply(client, V.RV_RESOURCE_IMPORT, new Array(10).fill(0));
      }
      if (!found.holders.has(client)) {
        if (!ensureCtx(client) || !ring32(OP.RES_ATTACH, client.ctx, [found.res], false)) {
          return false;
        }
        found.holders.add(client);
        client.resources.add(found.res);
        stats.imports = (stats.imports || 0) + 1;
      }
      return reply(client, V.RV_RESOURCE_IMPORT, [found.res, ...found.info, found.size]);
    }
    case V.RV_FENCE: {
      const fd = sys2(SYS.eventfd2, 0, O_CLOEXEC);
      if (fd < 0) {
        log("gpuserver: eventfd: error " + -fd);
        disconnect(client, "no eventfd");
        return "gone";
      }
      const f = { client, ctx: client.ctx, target: client.last, fd };
      if (!reply(client, V.RV_FENCE, [])) {
        sys1(SYS.close, fd);
        return "gone";
      }
      const ok = sendFd(client, fd);
      if (!f.target || execGone || reached(word(R.CTX_DONE + 4 * f.ctx), f.target)) {
        fenceSignal(f);
      } else {
        fences.push(f);
      }
      return ok ? true : "gone";
    }
    case V.TRANSFER_GET2:
    case V.TRANSFER_PUT2: {
      if (!ensureCtx(client)) {
        return false;
      }
      // handle level x y z width height depth data_size offset
      const r = resources.get(arg(0));
      if (!r || !r.size || arg(9) >= r.size) {
        log("gpuserver: " + client.name + ": transfer on resource " + arg(0) + " that has no such backing");
        return true;
      }
      const list = [arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7), arg(9), arg(8)];
      const s = ring32(cmd === V.TRANSFER_GET2 ? OP.TRANSFER_GET : OP.TRANSFER_PUT, client.ctx, list, true);
      if (!s) {
        return false;
      }
      client.last = s;
      return true;
    }
    case V.SUBMIT_CMD: {
      if (!ensureCtx(client)) {
        return false;
      }
      if (4 * len > MAX_SUBMIT) {
        disconnect(client, "a command buffer of " + 4 * len + " bytes (at most " + MAX_SUBMIT + ")");
        return "gone";
      }
      const s = record(OP.SUBMIT, client.ctx, 4 * len, (to) => u8().copyWithin(to, args, args + 4 * len), true);
      if (!s) {
        return false;
      }
      client.last = s;
      stats.submits++;
      return true;
    }
    default:
      if (vdec.isVideo(cmd)) {
        return vdec.command(client, cmd, args);
      }
      log("gpuserver: " + client.name + " sent command " + cmd + " (" + (NAMES[cmd] || "unknown") + "), which is not done here");
      disconnect(client, "unknown command " + cmd);
      return "gone";
    }
  };
  const ring32Bytes = (op, ctx, address, length) =>
    record(op, ctx, length, (to) => u8().copyWithin(to, address, address + length), false);

  // A virgl client's context, made with its first record (false: no room in the ring now).
  const ensureCtx = (client) => {
    if (client.created) {
      return true;
    }
    const name = client.nameBytes || new Uint8Array(1);
    if (!record(OP.CTX_CREATE, client.ctx, name.length, (to) => u8().set(name, to), false)) {
      return false;
    }
    client.created = true;
    return true;
  };

  // RESOURCE_CREATE_BLOB of a venus client: the pages, then the record, then the reply (the record is in
  // the ring before the client can use the blob, so the page knows it first).
  const venusBlob = (client, arg) => {
    const size = arg(2) + arg(3) * 0x100000000;
    const b = backing(size);
    if (b.error) {
      log("gpuserver: no blob of " + size + " bytes for " + client.name + " (" + b.what + ": error " + b.error + ")");
      disconnect(client, "out of memory");
      return "gone";
    }
    const res = nextRes++;
    const list = [res, arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), b.address % 0x100000000,
      Math.floor(b.address / 0x100000000)];
    if (!ring32(OP.VN_BLOB_CREATE, client.ctx, list, false)) {
      letGo(b);
      nextRes--;
      return false;
    }
    resources.set(res, { res, ctx: client.ctx, size, address: b.address, backing: b, holders: new Set([client]),
      info: [0, 0, 0, size, 1, 1, 1, 0], exportSlot: exportAdd(res, client.ctx, b.dev, b.ino) });
    client.resources.add(res);
    if (!reply(client, V.RESOURCE_CREATE_BLOB, [res])) {
      return "gone";
    }
    const ok = sendFd(client, b.fd);
    sent(b);
    return ok ? true : "gone";
  };

  // SUBMIT_CMD2 -> VN_SUBMIT: {n, then per batch {cmd_off, cmd_size, sync_off, sync_count, ring_idx, flags}
  // (dwords of this payload), the command dwords, then per sync {addr_lo, addr_hi, value_lo, value_hi}}.
  // The client's batch headers are 6 dwords (Mesa 25.0) or 8 (virglrenderer's newer header); where the first
  // batch's commands start (1 + 6n or 1 + 8n) says which. Returns the record's serial, 0 (no room) or "bad".
  const submitCmd2 = (client, args, len) => {
    if (!len) {
      return serial || 1;
    }
    const v = dv();
    const w = (i) => v.getUint32(args + 4 * i, true);
    const n = w(0);
    const stride = n && len > 2 && w(2) === 1 + 8 * n ? 8 : 6;
    if (n > (len - 1) / stride) {
      return "bad";
    }
    let cmdDw = 0, syncN = 0;
    const batches = [];
    for (let b = 0; b < n; b++) {
      const h = 1 + stride * b;
      const flags = w(h), cmdOff = w(h + 1), cmdSize = w(h + 2), syncOff = w(h + 3), syncCount = w(h + 4);
      const ringIdx = flags & SUBMIT_CMD2_FLAG_RING_IDX ? w(h + 5) : 0;
      if (cmdOff > len || cmdSize > len - cmdOff || syncOff > len || syncCount > (len - syncOff) / 3) {
        return "bad";
      }
      batches.push({ flags, cmdOff, cmdSize, syncOff, syncCount, ringIdx });
      cmdDw += cmdSize;
      syncN += syncCount;
    }
    const outDw = 1 + 6 * n + cmdDw + 4 * syncN;
    return record(OP.VN_SUBMIT, client.ctx, 4 * outDw, (to) => {
      const o = dv();
      o.setUint32(to, n, true);
      let cmdAt = 1 + 6 * n, syncAt = cmdAt + cmdDw;
      batches.forEach((bt, b) => {
        const h = to + 4 * (1 + 6 * b);
        [cmdAt, bt.cmdSize, syncAt, bt.syncCount, bt.ringIdx, bt.flags].forEach((x, i) => o.setUint32(h + 4 * i, x, true));
        u8().copyWithin(to + 4 * cmdAt, args + 4 * bt.cmdOff, args + 4 * (bt.cmdOff + bt.cmdSize));
        cmdAt += bt.cmdSize;
        for (let k = 0; k < bt.syncCount; k++) {
          const s = bt.syncOff + 3 * k;
          const id = syncOf(client, w(s));
          const value = (BigInt(w(s + 2)) << 32n) | BigInt(w(s + 1));
          // (a sync that is not the client's: its value goes to word 0, which nobody reads)
          const addr = syncAddr(id);
          if (id && value > syncPending[id]) {
            syncPending[id] = value;
          }
          const at = to + 4 * syncAt;
          o.setUint32(at, addr % 0x100000000, true);
          o.setUint32(at + 4, Math.floor(addr / 0x100000000), true);
          o.setUint32(at + 8, w(s + 1), true);
          o.setUint32(at + 12, w(s + 2), true);
          syncAt += 4;
        }
      });
    }, true);
  };

  // Take whole commands out of what a client sent, while it is not waiting.
  const work = (client) => {
    while (!client.gone) {
      if (client.length < 8) {
        return;
      }
      const v = dv();
      const len = v.getUint32(client.input, true);
      const cmd = v.getUint32(client.input + 4, true);
      const size = SIZES[cmd] !== undefined ? SIZES[cmd] : vdec.SIZES[cmd];
      const bytes = size === undefined ? 0 : size === -1 ? 4 * len : size === -2 ? len : 4 * size;
      if (size === -1 && 4 * len > MAX_SUBMIT) {
        disconnect(client, "a command of " + 4 * len + " bytes");
        return;
      }
      if (client.length < 8 + bytes) {
        return;
      }
      if (gs.debug && !client.wait) {
        log("gpuserver: " + client.fd + " " + (NAMES[cmd] || cmd) + " len " + len);
      }
      const r = command(client, cmd, len, client.input + 8);
      if (r === "gone" || client.gone) {
        return;
      }
      if (r === false) {
        return;	// again on a later turn, with the same bytes
      }
      const used = 8 + bytes;
      u8().copyWithin(client.input, client.input + used, client.input + client.length);
      client.length -= used;
    }
  };

  const receive = (client) => {
    if (!client.input) {
      client.input = alloc(IN_SIZE);
    }
    for (;;) {
      const free = IN_SIZE - client.length;
      if (free === 0) {
        return;	// (a full buffer: work() makes room)
      }
      const n = sys3(SYS.read, client.fd, client.input + client.length, free);
      if (n === -EINTR) {
        continue;
      }
      if (n === -EAGAIN) {
        return;
      }
      if (n <= 0) {
        disconnect(client, n === 0 ? "closed" : "read error " + -n);
        return;
      }
      client.length += n;
      work(client);
      if (client.gone || client.wait) {
        return;
      }
    }
  };

  // ---- video decoding (vdec.js): it uses these
  Object.assign(gs, { pid, backing, letGo, sent, sendFd, reply, disconnect });
  const vdec = gs.load("vdec.js");

  // ---- turns
  const channel = new MessageChannel();
  let turning = false, lastAnnounce = 0, lastStats = performance.now();
  const statsOn = env("RVGPU_STATS") === "1";
  let announcer = null;
  try {
    announcer = gs.channel("rv-gpu");
    // Whoever comes later (the window server, the page's executor) asks instead of waiting a second.
    announcer.onmessage = (e) => {
      if (e.data && e.data.type === "gpu?") {
        announce();
      }
    };
  } catch (error) {
    log("gpuserver: no BroadcastChannel here (" + error.message + "): the page will not find the ring");
  }
  const announce = () => {
    if (announcer) {
      announcer.postMessage({ type: "gpu", version: 2, pid, ring, ringSize: HEADER + DATA_SIZE, caps, capsSize: CAPS_SIZE,
        exports: exportsAt, exportsCap: EXPORTS_CAP, socket: socketPath, vdec: vdec.ring });
    }
  };

  const turn = () => {
    // Someone waits for the executor: wait on its `done` word a little instead of sleeping a whole turn.
    let waiting = fences.length > 0 || syncWaits.length > 0;
    for (const c of clients.values()) {
      if (c.length >= 8) {
        waiting = true;
        break;
      }
    }
    if (waiting) {
      doorbell();
      const done = word(R.DONE);
      Atomics.wait(i32(), (ring + R.DONE) >> 2, done | 0, 1);
    }
    const n = sys6(SYS.epoll_pwait, epfd, events, MAX_EVENTS, waiting ? 0 : TURN_MS, 0, 0);
    if (n < 0 && n !== -EINTR) {
      throw new Error("epoll_pwait: error " + -n);
    }
    const v = dv();
    for (let i = 0; i < n; i++) {
      const ready = v.getUint32(events + 16 * i, true);
      const fd = Number(v.getBigUint64(events + 16 * i + 8, true));
      if (fd === listener) {
        accept();
        continue;
      }
      const client = clients.get(fd);
      if (client && (ready & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
        receive(client);
      }
    }
    // Clients with commands left over (waiting for caps, room or their work): try again.
    for (const client of clients.values()) {
      if (client.length >= 8) {
        work(client);
        if (!client.gone && client.length < IN_SIZE) {
          receive(client);
        }
      }
    }
    doorbell();
    unmapTurn();
    fenceTurn();
    vdec.turn(performance.now());
    if (syncWaits.length) {
      syncWaitTurn();
    }
    setWord(R.HEARTBEAT, word(R.HEARTBEAT) + 1);
    const t = performance.now();
    execCheck(t);
    if (execGone) {
      for (const client of [...clients.values()]) {
        if (client.length >= 8) {
          disconnect(client, "the executor is gone");
        }
      }
      // Nothing more to do without an executor: go, taking the socket with us, so that the next
      // `RVGPU=1 desktop wayland` starts a new server, which a new executor (the page reloaded) can find.
      if (clients.size === 0) {
        log("gpuserver: no executor and no clients: done");
        unlinkAll();
        sys3(SYS.unlinkat, AT_FDCWD, path(socketPath, P0), 0);
        setReady(false);
        setNone(false);
        gs.exit(0);
      }
    }
    if (t - lastAnnounce > 1000) {
      lastAnnounce = t;
      announce();
    }
    if (statsOn && t - lastStats > 5000) {
      const dt = (t - lastStats) / 1000;
      log("stats: " + (stats.records / dt).toFixed(0) + " records/s, " + (stats.bytes / dt / 1024).toFixed(0) + " KiB/s, "
          + (stats.submits / dt).toFixed(1) + " submits/s, " + stats.waits + " waits, "
          + (stats.waits ? (stats.waitMs / stats.waits).toFixed(2) : "0") + " ms each, " + clients.size + " clients, "
          + resources.size + " resources");
      lastStats = t;
      stats.records = stats.bytes = stats.submits = stats.waits = stats.waitMs = 0;
    }
  };

  channel.port1.onmessage = () => {
    turning = false;
    try {
      turn();
    } catch (error) {
      if (error && error.name === "Trap") {
        gs.host.error(error);
        return;
      }
      gs.fatal(error);
      return;
    }
    again();
  };
  const again = () => {
    if (!turning) {
      turning = true;
      channel.port2.postMessage(0);
    }
  };

  log("gpuserver: listening on " + socketPath + " (ring at " + ring + ", " + DATA_SIZE / 1048576 + " MiB)");
  announce();
  again();
})
