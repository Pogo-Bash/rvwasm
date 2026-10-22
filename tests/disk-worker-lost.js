#!/usr/bin/env node
// tools/www/disk-worker.js when the browser takes the origin's private file system away (or runs out of room for it).
// No browser: the worker's source runs in a Node worker thread with stand-ins for what the browser gives it (as in
// tests/disk-worker-pool.js): synchronous XMLHttpRequest, fetch, Worker, and an OPFS in memory that can be "removed" the
// way Chrome 154 removes a private window's (measured with tools/storage-probe.js): every read() and write() of a handle
// from before then returns 4294967292 (-4 as an unsigned 32-bit number), getSize() returns 2^64 - 4, a directory handle
// from before and the root's getDirectoryHandle say NotFoundError. Another stand-in returns 4294967288 (-8, no room)
// from write() beyond a number of bytes. The main thread plays the kernel (reads and writes through the request words)
// and listens to the BroadcastChannel "rvwasm-storage" that the page shows its note from.
//
//   node tests/disk-worker-lost.js [NAME-PART]
"use strict";
const { Worker: NodeWorker, MessageChannel, isMainThread, parentPort, workerData } = require("node:worker_threads");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const vm = require("node:vm");

const PIECE = 4096;
const BASE = "disk/t/", STORE = "https://disk.test/disk/", ORIGIN = "https://site.test";
const piece_bytes = (i, version = 0) => {
  const bytes = Buffer.alloc(PIECE, (i * 7 + 3 + version * 100) & 255);
  bytes.writeUInt32LE(1000 + i, 0);
  return bytes;
};
const hash_of = (bytes) => crypto.createHash("sha256").update(bytes).digest("hex");
const range = (from, to) => Array.from({ length: to - from }, (_, k) => from + k);

// name -> { pieces, keep, query, opfs: false | { room }, drive(h), check(h) }
const scenarios = {
  "a disk of the image: the files are removed mid-session, reads go on from the server, written pieces from memory": {
    pieces: 24, query: "?v=1&fetchers=0",
    async drive(h) {
      for (const i of range(0, 10)) await h.read(i);
      await h.write(3, 5);                       // piece 3 is the machine's own now
      await h.write(4, 6);
      h.requests_before = await h.count();
      await h.remove_files();                    // the browser takes the storage away
      for (const i of range(0, 24)) await h.read(i);     // all of them, again (nine are new)
      await h.sleep(100);
    },
    check(h) {
      const after = h.xhr().length - h.requests_before;
      if (h.errors().length) return "errors in the log: " + h.errors().join(" | ");
      if (h.lost_lines().length != 1) return "expected one line about the lost storage, got " + h.lost_lines().length + ": " + h.logs.join(" | ");
      const lost = h.channel.filter((m) => m.type == "lost");
      if (lost.length != 1) return "expected one message on the channel, got " + lost.length;
      if (lost[0].kept != 2 || lost[0].fetched_again != 8 || lost[0].gone != 0) return "counts " + JSON.stringify(lost[0]);
      if (after != 22) return "expected the 22 pieces that are not written to fetched (8 read before + 14 new... less the 2 written), got " + after;
      return true;
    },
    async after(h) {
      // the written pieces are what the machine wrote (5 and 6), not the image's
      const a = await h.read_raw(3), b = await h.read_raw(4);
      return (a.every((x) => x == 5) && b.every((x) => x == 6)) || "written pieces came back as " + a[0] + "," + b[0];
    } },

  "a disk of the image with a fetcher running: the pool is stopped, nothing breaks": {
    pieces: 40, query: "?v=1&fetchers=3", manifest: { boot: range(10, 40) },
    async drive(h) {
      await h.read(0);
      await h.sleep(150);
      await h.remove_files();
      for (const i of range(0, 40)) await h.read(i);
      await h.sleep(300);
    },
    check(h) {
      if (h.errors().length) return "errors in the log: " + h.errors().join(" | ");
      if (h.lost_lines().length != 1) return "lost lines: " + h.lost_lines().length;
      return true;
    } },

  "a disk that is somebody's own, from its first start: after the removal it is the copy in memory, nothing is asked of the server": {
    pieces: 24, keep: true, query: "?v=1&fetchers=0",
    async drive(h) {
      await h.write(2, 9);
      h.requests_before = await h.count();
      await h.remove_files();
      await h.write(5, 8);
      for (const i of range(0, 24)) await h.read(i, i == 2 ? 9 : i == 5 ? 8 : null);
      await h.sleep(100);
    },
    check(h) {
      if (h.errors().length) return "errors in the log: " + h.errors().join(" | ");
      if (h.xhr().length != h.requests_before) return "the server was asked " + (h.xhr().length - h.requests_before) + " times after the removal";
      const lost = h.channel.filter((m) => m.type == "lost");
      if (lost.length != 1 || !lost[0].keep || lost[0].kept != 24 || lost[0].gone != 0) return "message " + JSON.stringify(lost);
      return true;
    } },

  "a disk that is somebody's own, from an earlier visit: all of it was copied to memory at the start": {
    pieces: 24, keep: true, seed: "keep", query: "?v=1&fetchers=0",
    async drive(h) {
      h.requests_before = await h.count();
      await h.remove_files();
      for (const i of range(0, 24)) await h.read(i);
      await h.sleep(100);
    },
    check(h) {
      if (h.errors().length) return "errors in the log: " + h.errors().join(" | ");
      if (h.xhr().length != h.requests_before) return "the server was asked after the removal";
      const lost = h.channel.filter((m) => m.type == "lost");
      if (lost.length != 1 || lost[0].kept != 24 || lost[0].gone != 0) return "message " + JSON.stringify(lost);
      return true;
    } },

  "removed while nobody is reading: the worker finds out by itself": {
    pieces: 8, query: "?v=1&fetchers=0",
    async drive(h) {
      await h.read(1);
      await h.remove_files();
      await h.until(() => h.channel.some((m) => m.type == "lost"), 9000);
      await h.sleep(50);
    },
    check(h) { return h.lost_lines().length == 1 || "lost lines: " + h.lost_lines().length; } },

  "the files fail for want of room: new pieces are kept in memory, the cache is told, nothing is lost": {
    pieces: 24, query: "?v=1&fetchers=0", opfs: { room: 6 * PIECE },
    async drive(h) {
      for (const i of range(0, 24)) await h.read(i);
      for (const i of range(0, 24)) await h.read(i);
      await h.sleep(100);
    },
    check(h) {
      if (h.errors().length) return "errors in the log: " + h.errors().join(" | ");
      const room = h.logs.filter((l) => /cannot store more of the disk/.test(l));
      if (room.length != 1) return "'cannot store more' lines: " + room.length;
      if (h.lost_lines().length) return "taken for a loss";
      const no = h.channel.filter((m) => m.type == "nospace");
      return no.length == 1 || "nospace messages: " + no.length;
    } },

  "a disk that fails in the same way at every request: one line, then a count now and then": {
    pieces: 24, query: "?v=1&fetchers=0", opfs: false, answer: (i) => (i == 5 ? { status: 404 } : null),
    async drive(h) {
      for (let n = 0; n < 30; n++) await h.read_fails(5);
    },
    check(h) {
      const lines = h.errors();
      if (lines.length < 1 || lines.length > 4) return lines.length + " error lines for 30 failures: " + lines.join(" | ");
      return true;
    } },

  "no files at all (this visit only): pieces that are only fetched are let go beyond a limit and fetched again": {
    pieces: 200, query: "?v=1&fetchers=0", opfs: false,
    async drive(h) {
      for (const i of range(0, 200)) await h.read(i);
      h.requests_before = await h.count();
      await h.read(199);       // the most recent: still here
      h.recent = (await h.count()) - h.requests_before;
      await h.read(0);         // the oldest: let go
      h.oldest = (await h.count()) - h.requests_before - h.recent;
      await h.write(1, 3);     // written: never let go
      for (const i of range(0, 200)) if (i != 1) await h.read(i);
      await h.read(1, 3);
    },
    check(h) {
      if (h.recent != 0) return "the latest piece was fetched again";
      if (h.oldest != 1) return "the oldest piece was not fetched again (" + h.oldest + ")";
      return true;
    } },
};

// ---------------------------------------------------------------------------------------------------------------
if (!isMainThread) {
  const { name, port, manifest, memory, control, hashes, killed, seed } = workerData;
  const sc = scenarios[name];
  globalThis.self = globalThis;
  globalThis.name = "disk t";
  globalThis.location = new URL(ORIGIN + "/disk-worker.js" + sc.query);
  // (The worker serves in a loop that never returns to the event loop: the files are handed over when it says it is ready.)
  globalThis.postMessage = (m) => {
    if (m.method == "ready" && workerData.dump_on_ready) {
      parentPort.postMessage({ dump: Object.fromEntries(Object.entries(files).map(([k, f]) => [k, Uint8Array.from(f.buf)])) });
    }
    parentPort.postMessage(m);
  };
  globalThis.onmessage = null;
  const source = fs.readFileSync(path.join(__dirname, "../tools/www/disk-worker.js"), "utf8");
  const GONE = 4294967292, NO_ROOM = 4294967288;
  const body_of = (i) => { const b = piece_bytes(i); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); };
  globalThis.XMLHttpRequest = class {
    open(method, url, async_) { this.url = url; if (async_ !== false) throw new Error("synchronous requests only"); }
    getResponseHeader() { return null; }
    send() {
      const i = this.url.startsWith(STORE) ? hashes.indexOf(this.url.slice(STORE.length, -4)) : Number(this.url.slice(this.url.lastIndexOf("/") + 1));
      port.postMessage({ ev: "xhr", piece: i, storage: this.url.startsWith(STORE) });
      const r = (sc.answer && sc.answer(i)) || { status: 200 };
      this.status = r.status;
      this.response = r.status == 200 ? body_of(i) : new ArrayBuffer(0);
    }
  };
  globalThis.fetch = async () => ({ json: async () => manifest });
  globalThis.Worker = class {
    constructor() { this.onmessage = null; }
    postMessage() {}
    terminate() {}
  };
  const files = {};
  for (const [k, v] of Object.entries(seed || {})) files[k] = { name: k, buf: Buffer.from(v) };
  const dead = () => Atomics.load(killed, 0) == 1;
  const file_handle = (file) => ({
    getSize: () => (dead() ? 2 ** 64 - 4 : file.buf.length),
    truncate: (n) => { if (dead()) return; const b = Buffer.alloc(n); file.buf.copy(b, 0, 0, Math.min(n, file.buf.length)); file.buf = b; },
    read: (view, { at }) => {
      if (dead()) return GONE;
      const n = Math.max(0, Math.min(view.byteLength, file.buf.length - at));
      new Uint8Array(view.buffer, view.byteOffset, view.byteLength).set(file.buf.subarray(at, at + n));  // (bytes, whatever the view's type)
      return n;
    },
    write: (view, { at }) => {
      if (dead()) return GONE;
      const end = at + view.byteLength;
      if (sc.opfs && file.name == "data" && end > sc.opfs.room) return NO_ROOM;
      if (end > file.buf.length) { const b = Buffer.alloc(end); file.buf.copy(b); file.buf = b; }
      file.buf.set(new Uint8Array(view.buffer, view.byteOffset, view.byteLength), at);
      return view.byteLength;
    },
    flush: () => {},
    close: () => {},
  });
  if (sc.opfs === false) {
    Object.defineProperty(globalThis, "navigator", { configurable: true, value: { storage: { getDirectory: async () => { throw Object.assign(new Error("no"), { name: "NotSupportedError" }); } } } });
  } else {
    const directory = {
      getFileHandle: async (n) => {
        if (dead()) throw Object.assign(new Error("A requested file or directory could not be found"), { name: "NotFoundError" });
        const file = files[n] || (files[n] = { name: n, buf: Buffer.alloc(0) });
        return { createSyncAccessHandle: async () => file_handle(file) };
      },
    };
    Object.defineProperty(globalThis, "navigator", { configurable: true, value: { storage: { getDirectory: async () => ({
      getDirectoryHandle: async () => {
        if (dead()) throw Object.assign(new Error("A requested file or directory could not be found"), { name: "NotFoundError" });
        return directory;
      } }) } } });
  }
  vm.runInThisContext(source);
  globalThis.onmessage({ data: { memory, control, url: BASE, name: "t", keep: !!sc.keep, reset: !seed } });
} else {
  const filter = process.argv[2] || "";
  const run = (name, sc, seed) => new Promise((resolve, reject) => {
    const n = sc.pieces;
    const pieces = range(0, n).map((i) => piece_bytes(i));
    const hashes = pieces.map(hash_of);
    const manifest = { size: PIECE * n, piece: PIECE, id: "t", present: "f".repeat(Math.ceil(n / 4)), hashes, base: STORE, ...(sc.manifest || {}) };
    const memory = new WebAssembly.Memory({ initial: 2, maximum: 2, shared: true });
    const control = new Int32Array(new SharedArrayBuffer(64));
    const killed = new Int32Array(new SharedArrayBuffer(4));
    const events = [], logs = [], channel = [];
    const mc = new MessageChannel();
    mc.port1.on("message", (m) => events.push(m));
    mc.port1.unref();
    const bc = new BroadcastChannel("rvwasm-storage");
    bc.onmessage = (e) => channel.push(e.data);
    const request = async (op, i, fill, expect) => {
      const length = PIECE;
      if (op == 1) new Uint8Array(memory.buffer, 0, length).fill(fill);
      control[3] = op; control[4] = (i * PIECE) / 512; control[5] = 0;
      control[6] = 0; control[11] = 0; control[7] = length / 512;
      Atomics.store(control, 2, 0); Atomics.store(control, 1, 1); Atomics.notify(control, 1);
      while (Atomics.load(control, 2) == 0) await Atomics.waitAsync(control, 2, 0, 1000).value;
      return control[8];
    };
    const h = {
      logs, channel,
      sleep: (ms) => new Promise((r) => setTimeout(r, ms)),
      until: async (pred, ms) => { const end = Date.now() + ms; while (!pred()) { if (Date.now() > end) throw new Error("timed out waiting"); await new Promise((r) => setTimeout(r, 20)); } },
      count: async () => { await new Promise((r) => setTimeout(r, 60)); return events.filter((e) => e.ev == "xhr").length; },  // (the reports come over a port: a moment)
      xhr: () => events.filter((e) => e.ev == "xhr"),
      errors: () => logs.filter((l) => /^error:/.test(l)),
      lost_lines: () => logs.filter((l) => /removed this site's storage/.test(l)),
      remove_files: async () => { Atomics.store(killed, 0, 1); },
      // a read that must give the piece as it is (or as `fill` says it was written)
      read: async (i, fill) => {
        const r = await request(0, i);
        if (r != 0) throw new Error("read of piece " + i + " failed: " + r);
        const got = Buffer.from(new Uint8Array(memory.buffer, 0, PIECE));
        const want = fill == null || fill === undefined ? (h.written[i] !== undefined ? Buffer.alloc(PIECE, h.written[i]) : pieces[i]) : Buffer.alloc(PIECE, fill);
        if (!got.equals(want)) throw new Error("piece " + i + " differs (first byte " + got[0] + ", wanted " + want[0] + ")");
      },
      read_raw: async (i) => { const r = await request(0, i); if (r) throw new Error("read failed " + r); return Buffer.from(new Uint8Array(memory.buffer, 0, PIECE)); },
      read_fails: async (i) => { const r = await request(0, i); if (r == 0) throw new Error("expected a failure"); },
      written: {},
      write: async (i, fill) => { const r = await request(1, i, fill); if (r != 0) throw new Error("write of piece " + i + " failed: " + r); h.written[i] = fill; },
    };
    const start = (seed_files) => new NodeWorker(__filename, { workerData: { name, port: mc.port2, manifest, memory, control, hashes, killed, seed: seed_files }, transferList: [mc.port2] });
    const w = start(seed);
    w.on("error", reject);
    w.on("message", async (m) => {
      if (m.method == "log") logs.push(m.text);
      else if (m.method == "failed") reject(new Error("worker failed: " + m.text));
      else if (m.dump) { h.dump = m.dump; }
      else if (m.method == "ready") {
        try {
          await sc.drive(h);
          await h.sleep(60);
          let verdict = sc.check(h);
          if (verdict === true && sc.after) verdict = await sc.after(h);
          w.terminate();
          bc.close();
          resolve(verdict);
        } catch (e) { w.terminate(); bc.close(); reject(e); }
      }
    });
    setTimeout(() => reject(new Error("the scenario did not finish in 60 s")), 60000).unref();
  });

  // (A disk "from an earlier visit": the files a first start leaves, handed to the next worker.)
  const first_start = (name, sc) => new Promise((resolve, reject) => {
    const n = sc.pieces;
    const pieces = range(0, n).map((i) => piece_bytes(i));
    const hashes = pieces.map(hash_of);
    const manifest = { size: PIECE * n, piece: PIECE, id: "t", present: "f".repeat(Math.ceil(n / 4)), hashes, base: STORE };
    const memory = new WebAssembly.Memory({ initial: 2, maximum: 2, shared: true });
    const control = new Int32Array(new SharedArrayBuffer(64));
    const killed = new Int32Array(new SharedArrayBuffer(4));
    const mc = new MessageChannel();
    mc.port1.on("message", () => {});
    mc.port1.unref();
    const w = new NodeWorker(__filename, { workerData: { name, port: mc.port2, manifest, memory, control, hashes, killed, seed: null, dump_on_ready: true }, transferList: [mc.port2] });
    w.on("error", reject);
    w.on("message", (m) => {
      if (m.dump) { w.terminate(); resolve(m.dump); }
    });
  });

  (async () => {
    let failed = 0, ran = 0;
    for (const [name, sc] of Object.entries(scenarios)) {
      if (filter && !name.includes(filter)) continue;
      ran++;
      let line;
      try {
        const seed = sc.seed ? await first_start(name, sc) : null;
        const verdict = await run(name, sc, seed);
        line = (verdict === true ? "ok   " : "FAIL ") + name + (verdict === true ? "" : ": " + verdict);
        if (verdict !== true) failed++;
      } catch (e) {
        line = "FAIL " + name + ": " + e.message;
        failed++;
      }
      console.log(line);
    }
    console.log(failed ? failed + " of " + ran + " failed" : "all " + ran + " passed");
    process.exit(failed ? 1 : 0);
  })();
}
