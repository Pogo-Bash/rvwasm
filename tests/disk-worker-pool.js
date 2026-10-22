#!/usr/bin/env node
// tools/www/disk-worker.js fetching ahead (the pool): the fetcher worker, the boot list, read-ahead, priorities.
// No browser: the disk worker and its fetcher (the same file, started by the disk worker as a nested worker named
// "disk-fetcher") run as Node worker threads with stand-ins for what the browser gives them: synchronous
// XMLHttpRequest (the disk worker's own requests), fetch (the fetcher's), the origin's private file system (OPFS) and
// Worker. Every request is reported with the time it began and ended, so the tests read how many were in flight at
// once, in what order, and whether a piece was asked for twice. The main thread plays the kernel: it reads pieces
// through the request words, as the machine does.
//
//   node tests/disk-worker-pool.js [NAME-PART]
"use strict";
const { Worker: NodeWorker, MessageChannel, isMainThread, parentPort, workerData } = require("node:worker_threads");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const vm = require("node:vm");

const PIECE = 4096, PIECES = 64, SIZE = PIECE * (PIECES - 1) + 1024;   // (the last piece is short)
const BASE = "disk/t/", STORE = "https://disk.test/disk/", ORIGIN = "https://site.test";
const SLOTS = 24;
const now = () => performance.timeOrigin + performance.now();
const piece_bytes = (i) => {
  const length = i == PIECES - 1 ? SIZE - PIECE * i : PIECE;
  const bytes = Buffer.alloc(length, (i * 7 + 3) & 255);
  bytes.writeUInt32LE(1000 + i, 0);
  return bytes;
};
const hash_of = (bytes) => crypto.createHash("sha256").update(bytes).digest("hex");
const range = (from, to) => Array.from({ length: to - from }, (_, k) => from + k);

// name -> what the stand-ins do and what must have happened. Fields:
//   query     the disk worker's address (?fetchers=3 ...); manifest: extra manifest fields; keep: the home disk
//   fetch     (i, n) -> { status, headers, delay, throws, hang } for the fetcher's n-th request (default 200 after `delay` ms)
//   xhr       the disk worker's synchronous requests: { delay } before they answer, and answer(url, i) -> { status }
//   drive     async (h) the kernel's part; check (h) -> true or what is wrong
const scenarios = {
  "boot list: in order, the pool bounded, every piece once, none from the site": {
    query: "?v=1&fetchers=3", manifest: { boot: range(10, 50) }, delay: 25,
    async drive(h) { await h.until(() => h.fetched().length >= 40, 8000); await h.sleep(100); },
    check(h) {
      const starts = h.fetched();
      if (starts.join() != range(10, 50).join()) return "fetch order " + starts;
      if (h.max_flying() != 3) return "at most 3 at once expected to reach exactly 3, saw " + h.max_flying();
      if (h.xhr().length) return "the disk worker asked for " + h.xhr().length + " pieces itself";
      if (h.site().length) return "the site was asked";
      return true;
    },
    async after(h) {   // the pieces are in the files: reading them asks for nothing, and they are right
      for (const i of range(10, 50)) await h.read(i);
      // (reading 40 pieces in a row also starts read-ahead beyond 49: that is not "again")
      return h.xhr().length == 0 && h.fetched().filter((i) => i < 50).length == 40 || "reading them fetched again";
    } },

  "boot list: only the part not here yet, and nothing outside the disk": {
    query: "?v=1&fetchers=4", manifest: { boot: [3, 3, 4, 999, -1, 1.5, 5, 0] }, delay: 10,
    async drive(h) { await h.until(() => h.fetched().length >= 4, 4000); await h.sleep(100); },
    check(h) {
      const got = h.fetched().slice().sort((a, b) => a - b).join();
      return got == "0,3,4,5" || "fetched " + got;
    } },

  "memory is bounded: never more than 24 pieces wait for the files, however slow the files are": {
    query: "?v=1&fetchers=6", manifest: { boot: range(0, 60) }, delay: 3, write_delay: 30,
    async drive(h) { await h.until(() => h.fetched().length >= 60, 15000); await h.sleep(300); },
    check(h) {
      // (fetched but not yet written, at every moment: the writes of `data` are the ones of a whole piece)
      let waiting = 0, worst = 0;
      for (const e of h.sorted()) {
        if (e.ev == "fetch" && e.phase == "end" && e.status == 200) waiting++;
        else if (e.ev == "write" && e.size == PIECE) waiting--;
        worst = Math.max(worst, waiting);
      }
      return worst <= SLOTS && worst >= 10 || "up to " + worst + " pieces waiting (limit " + SLOTS + ", and the test should fill it)";
    } },

  "a piece the kernel waits for jumps the queue: its own request at once, nothing new starts meanwhile": {
    query: "?v=1&fetchers=2", manifest: { boot: range(10, 50) }, delay: 100, xhr: { delay: 150 },
    async drive(h) {
      await h.sleep(250);
      h.mark("demand");
      await h.read(60);
      h.mark("demand done");
      await h.sleep(50);
    },
    check(h) {
      const x = h.xhr().filter((e) => e.piece == 60)[0];
      if (!x) return "no request for piece 60";
      const began = h.marks.demand;
      if (h.xhr_events(60, "start")[0].t - began > 40) return "the request began " + (h.xhr_events(60, "start")[0].t - began) + " ms after the read";
      // (what the pool had running goes on, up to its 2: the kernel's piece comes on top of that at once, and the pool starts
      // nothing that would make the two together more than 2)
      const during = h.events.filter((e) => e.ev == "fetch" && e.phase == "start" && e.t > h.xhr_events(60, "start")[0].t + 1 && e.t < h.xhr_events(60, "end")[0].t - 3);
      if (during.some((e) => e.inflight > 1)) return "the pool started a request while the kernel's piece was fetched and it had " + during.map((e) => e.inflight) + " going";
      if (h.max_total_flying_after(h.xhr_events(60, "start")[0].t) > 3) return "more than the limit and the one piece waited for were in flight";
      return true;
    } },

  "a piece still in the queue that the kernel wants is fetched once (by the disk worker), not again by the pool": {
    query: "?v=1&fetchers=2", manifest: { boot: range(10, 50) }, delay: 60, xhr: { delay: 20 },
    async drive(h) {
      await h.sleep(100);
      await h.read(45);
      await h.until(() => h.fetched().length >= 39, 8000);
      await h.sleep(150);
    },
    check(h) {
      const asked = h.requests_for(45);
      return asked == 1 || "piece 45 was asked for " + asked + " times";
    } },

  "a piece in flight that the kernel wants is waited for, not asked for twice": {
    query: "?v=1&fetchers=2", manifest: { boot: [5, 6, 7, 8] }, delay: 400, xhr: { delay: 20 },
    async drive(h) {
      await h.sleep(100);
      h.mark("read");
      await h.read(5);
      h.mark("read done");
      await h.sleep(100);
    },
    check(h) {
      const asked = h.requests_for(5);
      if (asked != 1) return "piece 5 was asked for " + asked + " times";
      if (h.marks["read done"] - h.marks.read < 200) return "the read did not wait for the request in flight (" + (h.marks["read done"] - h.marks.read) + " ms)";
      return true;
    } },

  "read-ahead: 4, then 8, then 16 pieces, on a run of consecutive pieces": {
    query: "?v=1&fetchers=6", delay: 10, xhr: { delay: 5 },
    async drive(h) {
      const steps = [];
      const settle = async () => { let n; do { n = h.fetched().length; await h.sleep(120); } while (h.fetched().length != n); };
      await h.read(20); await settle(); steps.push(h.fetched().slice());          // a piece alone: nothing ahead
      await h.read(21); await settle(); steps.push(h.fetched().slice());          // the second of a run: 4
      await h.read(22); await settle(); steps.push(h.fetched().slice());          // 8
      await h.read(23); await settle(); steps.push(h.fetched().slice());          // 16
      await h.read(24); await settle(); steps.push(h.fetched().slice());          // stays 16
      h.steps = steps;
    },
    check(h) {
      const top = (list) => list.length ? Math.max(...list) : 0;
      const s = h.steps;
      if (s[0].length) return "a lone piece was read ahead: " + s[0];
      if (s[1].slice().sort((a, b) => a - b).join() != "22,23,24,25") return "after the second piece: " + s[1];
      if (top(s[2]) != 22 + 8 || s[2].length != 9) return "after the third piece the top is " + top(s[2]) + " with " + s[2].length;
      if (top(s[3]) != 23 + 16) return "after the fourth piece the top is " + top(s[3]);
      if (top(s[4]) != 24 + 16) return "after the fifth piece the top is " + top(s[4]) + " (16 is the most)";
      if (new Set(h.fetched()).size != h.fetched().length) return "a piece was fetched twice";
      return true;
    } },

  "read-ahead: a jump starts again, two runs side by side both go on": {
    query: "?v=1&fetchers=6", delay: 10, xhr: { delay: 2 },
    async drive(h) {
      const settle = async () => { let n; do { n = h.fetched().length; await h.sleep(100); } while (h.fetched().length != n); };
      for (const i of [40, 50, 41, 51]) await h.read(i);
      await settle();
      await h.read(10); await settle();   // (alone)
    },
    check(h) {
      const got = h.fetched().slice().sort((a, b) => a - b).join();
      return got == "42,43,44,45,52,53,54,55" || "fetched " + got;
    } },

  "read-ahead and the boot list: a piece at the end of the boot list that a run reaches moves up, ahead of the rest": {
    query: "?v=1&fetchers=1", manifest: { boot: range(30, 60).concat([14, 15]) }, delay: 30, xhr: { delay: 2 },
    async drive(h) {
      await h.read(10); await h.read(11);   // a run: 12-15 are asked for (14 and 15 are queued already, at the end)
      await h.until(() => h.fetched().length >= 8, 6000);
    },
    check(h) {
      // the pool fetches one at a time: the read-ahead comes before the boot list's pieces that are not started
      const f = h.fetched();
      const at = (i) => f.indexOf(i);
      return [12, 13, 14, 15].every((i) => at(i) >= 0 && at(i) < at(33)) || "order " + f.join(" ");
    } },

  "prefetch is from the object storage only: a refusal is not made up from the site, the kernel's own request still falls back to it": {
    query: "?v=1&fetchers=2", manifest: { boot: [10, 11, 12, 13] }, delay: 10,
    fetch: (i) => i == 11 || i == 12 ? { status: 403 } : {},
    xhr: { answer: (url, i, storage) => storage && (i == 11 || i == 12) ? { status: 403 } : null },
    async drive(h) {
      await h.until(() => h.fetched().length >= 4, 4000); await h.sleep(150);
      if (h.site().length) throw new Error("the site was asked while nobody waited: " + h.site().map((e) => e.url));
      await h.read(11);
    },
    check(h) {
      if (h.fetched().filter((i) => i == 11).length != 1) return "a refused piece was tried " + h.fetched().filter((i) => i == 11).length + " times";
      return h.site().length == 1 && h.site()[0].piece == 11 || "site requests: " + h.site().map((e) => e.url);
    } },

  "the object storage down: a few tries, then the pool leaves it alone, and the kernel is served by the site": {
    query: "?v=1&fetchers=3", manifest: { boot: range(10, 40) }, delay: 5,
    fetch: () => ({ throws: true }),
    xhr: { answer: (url, i, storage) => storage ? { throws: true } : null },
    async drive(h) { await h.sleep(600); await h.read(10); await h.read(11); },
    check(h) {
      if (h.fetched().length > 8) return h.fetched().length + " tries on a storage that does not answer";
      return h.site().length >= 2 || "the kernel was not served by the site";
    } },

  "a Retry-After from the storage pauses the pool as long as it asks": {
    query: "?v=1&fetchers=3", manifest: { boot: range(10, 20) }, delay: 10,
    fetch: (i, n) => n == 1 ? { status: 429, headers: { "retry-after": "1" } } : {},
    async drive(h) { await h.until(() => h.fetched().length >= 11, 6000); await h.sleep(100); },
    check(h) {
      const starts = h.events.filter((e) => e.ev == "fetch" && e.phase == "start").sort((a, b) => a.t - b.t);
      const refused = h.events.find((e) => e.ev == "fetch" && e.phase == "end" && e.status == 429);
      if (!refused) return "no refusal seen";
      const after = starts.filter((e) => e.t > refused.t + 1);
      if (!after.length) return "nothing started after the refusal";
      const gap = after[0].t - refused.t;
      if (gap < 950) return "the pool came back after " + Math.round(gap) + " ms";
      if (h.requests_for(10) != 2) return "the refused piece was asked for " + h.requests_for(10) + " times (one more try expected)";
      return true;
    } },

  "the kernel's own request told to wait by the storage (Retry-After): the pool leaves the storage alone meanwhile, the site serves the kernel": {
    query: "?v=1&fetchers=3", manifest: { boot: range(30, 50) }, delay: 20,
    xhr: { answer: (url, i, storage) => storage && i == 5 ? { status: 429, headers: { "retry-after": "1" } } : null },
    async drive(h) {
      await h.sleep(60);
      await h.read(5);   // (not in the boot list: the disk worker's own request, refused, then the site's)
      h.mark("served");
      await h.sleep(1300);
    },
    check(h) {
      const refused = h.xhr_events(5, "end").find((e) => e.storage);
      if (!refused) return "no request to the storage for piece 5";
      if (h.site().filter((e) => e.piece == 5).length != 1) return "the site did not serve piece 5 once";
      const starts = h.events.filter((e) => e.ev == "fetch" && e.phase == "start" && e.t > refused.t + 2 && e.t < refused.t + 900);
      return starts.length == 0 || starts.length + " pool requests during the second the storage asked for";
    } },

  "the map is saved when the pieces are: every 64, and after a second of rest": {
    query: "?v=1&fetchers=4", manifest: { boot: range(0, 40) }, delay: 5,
    async drive(h) { await h.until(() => h.fetched().length >= 40, 6000); await h.sleep(1500); },
    check(h) {
      const writes = h.events.filter((e) => e.ev == "map");
      // (once when the files are made, once after the second of rest with 40 new pieces)
      return writes.length >= 2 && writes[writes.length - 1].t > h.sorted().filter((e) => e.ev == "write").pop().t + 900
        || "map writes at " + writes.map((e) => Math.round(e.t - h.began_at));
    } },

  "fetchers=0: no fetcher, the boot list is not used, everything is the disk worker's own": {
    query: "?v=1&fetchers=0", manifest: { boot: range(10, 20) }, xhr: { delay: 1 },
    async drive(h) { await h.sleep(200); await h.read(12); await h.read(13); await h.read(14); },
    check(h) {
      if (h.workers_started) return "a fetcher was started";
      return h.fetched().length == 0 && h.xhr().length == 3 || "fetched " + h.fetched().length + ", own requests " + h.xhr().length;
    } },

  "no object storage in the manifest: no fetcher work (the site's pieces are not fetched ahead)": {
    query: "?v=1", manifest: { boot: range(10, 20) }, no_remote: true, xhr: { delay: 1 },
    async drive(h) { await h.sleep(200); await h.read(12); await h.read(13); await h.read(14); },
    check(h) { return h.fetched().length == 0 && h.site().length == 3 || "fetched " + h.fetched().length + ", site " + h.site().length; } },

  "no private file system: nothing is fetched ahead (memory would be all there is)": {
    query: "?v=1&fetchers=6", manifest: { boot: range(10, 20) }, no_opfs: true, xhr: { delay: 1 },
    async drive(h) { await h.sleep(200); await h.read(12); await h.read(13); await h.read(14); await h.read(15); },
    check(h) { return h.fetched().length == 0 && h.xhr().length == 4 || "fetched " + h.fetched().length + ", own requests " + h.xhr().length; } },

  "a fetcher that does not come up: the disk goes on, one piece at a time": {
    query: "?v=1&fetchers=6", manifest: { boot: range(10, 20) }, deaf: true, xhr: { delay: 1 },
    async drive(h) { await h.read(12); await h.read(13); await h.read(14); },
    check(h) {
      if (!h.logs.some((l) => /no fetcher/.test(l))) return "no word of it";
      return h.xhr().length == 3 || "own requests " + h.xhr().length;
    } },

  "a fetcher whose request never answers: the kernel waits a while for it, then asks for itself": {
    query: "?v=1&fetchers=2&flying=300", manifest: { boot: [7, 8, 9] }, delay: 20,
    fetch: (i) => i == 7 ? { hang: true } : {},
    xhr: { delay: 5 },
    async drive(h) {
      await h.sleep(100);
      h.mark("read");
      await h.read(7);
      h.mark("read done");
      await h.read(8); await h.read(9);
    },
    check(h) {
      const took = h.marks["read done"] - h.marks.read;
      if (took < 250 || took > 1500) return "the wait was " + Math.round(took) + " ms";
      return h.xhr().filter((e) => e.piece == 7).length == 1 || "the kernel's own request for 7 was not made once";
    } },

  "the home disk's first start: its pieces copied several at a time, all right afterwards": {
    query: "?v=1&fetchers=6", keep: true, delay: 100, xhr: { delay: 100 }, pieces: 24,
    async drive(h) {
      h.copy_ms = h.ready_at - h.began_at;
      for (const i of range(0, 24)) await h.read(i);
    },
    check(h) {
      if (h.copy_ms > 1300) return "the copy took " + Math.round(h.copy_ms) + " ms (24 pieces of 100 ms one by one are 2400)";
      if (h.max_flying() < 4) return "never more than " + h.max_flying() + " at once";
      const all = h.fetched().length + h.xhr().length;
      if (all != 24) return all + " requests for 24 pieces";
      return h.requests_for_all_once() || "a piece was asked for twice";
    },
    async after(h) { return h.fetched().length + h.xhr().length == 24 || "reading them asked again"; } },

  "the home disk's first start with the fetcher gone: copied one by one, as it was": {
    query: "?v=1&fetchers=0", keep: true, xhr: { delay: 10 }, pieces: 24,
    async drive(h) { for (const i of range(0, 24)) await h.read(i); },
    check(h) { return h.xhr().length == 24 && h.fetched().length == 0 || "own requests " + h.xhr().length; } },

  // The browser takes the files away while the pool is writing what it fetched (tests/disk-worker-lost.js has the rest of the
  // storage that goes: here only the pool's part): after `kill_after` writes of pieces the handles answer 4294967292.
  "the files are taken away while pieces are fetched ahead: one line, the pool stops, every piece is still read right": {
    query: "?v=1&fetchers=3", manifest: { boot: range(10, 50) }, delay: 20, kill_after: 8,
    async drive(h) {
      await h.until(() => h.lost_lines().length > 0, 8000);
      await h.sleep(200);
      for (const i of range(0, 50)) await h.read(i);
    },
    check(h) {
      const errors = h.logs.filter((l) => /^error:/.test(l));
      if (errors.length) return "errors in the log: " + errors.join(" | ");
      if (h.lost_lines().length != 1) return "lines about the loss: " + h.lost_lines().length;
      const broke = h.logs.filter((l) => /fetching ahead stopped/.test(l));
      return !broke.length || "the pool broke instead of being stopped: " + broke.join(" | ");
    } },
};

// ---------------------------------------------------------------------------------------------------------------
if (!isMainThread) {
  const { role, name, port, manifest, memory, control, hashes } = workerData;
  const sc = scenarios[name];
  const by_hash = new Map(hashes.map((h, i) => [h, i]));
  globalThis.self = globalThis;
  globalThis.name = role == "disk" ? "disk t" : "disk-fetcher";
  globalThis.location = new URL(ORIGIN + "/disk-worker.js" + sc.query);
  globalThis.postMessage = (m) => parentPort.postMessage(m);
  globalThis.onmessage = null;
  const source = fs.readFileSync(path.join(__dirname, "../tools/www/disk-worker.js"), "utf8");
  const sleep_sync = (ms) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
  const body_of = (i) => {
    const b = piece_bytes(i);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
  };
  const index_of = (url) => url.startsWith(STORE) ? by_hash.get(url.slice(STORE.length, -4)) : Number(url.slice(url.lastIndexOf("/") + 1));

  if (role == "deaf") {
    // (a nested worker that never comes up)
  } else if (role == "fetcher") {
    let inflight = 0, n = 0;
    globalThis.fetch = async (url) => {
      const i = index_of(url);
      const spec = (sc.fetch && sc.fetch(i, ++n)) || {};
      inflight++;
      port.postMessage({ ev: "fetch", phase: "start", i, t: now(), inflight });
      let status = 0;
      try {
        if (spec.hang) await new Promise(() => {});
        await new Promise((resolve) => setTimeout(resolve, spec.delay ?? sc.delay ?? 20));
        if (spec.throws) throw new TypeError("Failed to fetch");
        status = spec.status ?? 200;
        return { status, headers: { get: (k) => (spec.headers || {})[k.toLowerCase()] ?? null }, arrayBuffer: async () => body_of(i) };
      } finally {
        inflight--;
        port.postMessage({ ev: "fetch", phase: "end", i, t: now(), status });
      }
    };
    vm.runInThisContext(source);
    parentPort.on("message", (m) => globalThis.onmessage({ data: m }));
  } else {
    // ---- the disk worker ----
    const xhr_spec = sc.xhr || {};
    globalThis.XMLHttpRequest = class {
      open(method, url, async_) { this.url = url; if (async_ !== false) throw new Error("the worker must use synchronous requests"); }
      getResponseHeader(k) { return this.headers[k.toLowerCase()] ?? null; }
      send() {
        const i = index_of(this.url);
        const storage = this.url.startsWith(STORE);
        port.postMessage({ ev: "xhr", phase: "start", url: this.url, piece: i, storage, t: now() });
        if (xhr_spec.delay) sleep_sync(xhr_spec.delay);
        const r = (xhr_spec.answer && xhr_spec.answer(this.url, i, storage)) || { status: 200 };
        port.postMessage({ ev: "xhr", phase: "end", url: this.url, piece: i, storage, t: now() });
        if (r.throws) throw new Error("NetworkError");
        this.status = r.status;
        this.headers = r.headers || {};
        this.response = r.status == 200 ? body_of(i) : new ArrayBuffer(0);
      }
    };
    globalThis.fetch = async () => ({ json: async () => manifest });
    // the nested worker: a thread with the same script
    globalThis.Worker = class {
      constructor(url, options) {
        port.postMessage({ ev: "worker", t: now() });
        const ch = workerData.fetcher_port;
        this.w = new NodeWorker(__filename, { workerData: { ...workerData, role: sc.deaf ? "deaf" : "fetcher", port: ch }, transferList: [ch] });
        this.w.on("message", (m) => this.onmessage && this.onmessage({ data: m }));
        this.w.on("error", (e) => this.onerror && this.onerror({ message: e.message }));
      }
      postMessage(m) { this.w.postMessage(m); }
      terminate() { this.w.terminate(); }
    };
    // the origin's private file system, in memory
    if (sc.no_opfs) {
      Object.defineProperty(globalThis, "navigator", { configurable: true, value: { storage: { getDirectory: async () => { throw Object.assign(new Error("no"), { name: "NotSupportedError" }); } } } });
    } else {
      let data_writes = 0;
      const dead = () => !!sc.kill_after && data_writes >= sc.kill_after;
      const handle = (file) => {
        const h = {
          getSize: () => (dead() ? 2 ** 64 - 4 : file.buf.length),
          truncate: (n) => { const b = Buffer.alloc(n); file.buf.copy(b, 0, 0, Math.min(n, file.buf.length)); file.buf = b; },
          read: (view, { at }) => { if (dead()) return 4294967292; const n = Math.max(0, Math.min(view.byteLength, file.buf.length - at)); new Uint8Array(view.buffer, view.byteOffset, view.byteLength).set(file.buf.subarray(at, at + n)); return n; },
          write: (view, { at }) => {
            if (dead()) return 4294967292;
            if (file.name == "data") data_writes++;
            if (file.name == "map" && at == 64) port.postMessage({ ev: "map", t: now() });
            if (file.name == "data") {
              if (sc.write_delay) sleep_sync(sc.write_delay);
              port.postMessage({ ev: "write", size: view.byteLength, t: now() });
            }
            const end = at + view.byteLength;
            if (end > file.buf.length) { const b = Buffer.alloc(end); file.buf.copy(b); file.buf = b; }
            file.buf.set(new Uint8Array(view.buffer, view.byteOffset, view.byteLength), at);
            return view.byteLength;
          },
          flush: () => {}, close: () => {},
        };
        return h;
      };
      const files = {};
      const directory = { getFileHandle: async (n) => { const file = files[n] || (files[n] = { name: n, buf: Buffer.alloc(0) }); return { createSyncAccessHandle: async () => handle(file) }; } };
      Object.defineProperty(globalThis, "navigator", { configurable: true, value: { storage: { getDirectory: async () => ({ getDirectoryHandle: async () => directory }) } } });
    }
    vm.runInThisContext(source);
    globalThis.onmessage({ data: { memory, control, url: BASE, name: "t", keep: !!sc.keep, reset: true } });
  }
} else {
  const filter = process.argv[2] || "";
  const run = (name, sc) => new Promise((resolve, reject) => {
    const n = sc.pieces || PIECES;
    const pieces = range(0, PIECES).map(piece_bytes);
    const hashes = pieces.map(hash_of);
    const manifest = { size: SIZE, piece: PIECE, id: "t", present: "ffffffffffffffff", hashes, base: sc.no_remote ? undefined : STORE, ...(sc.manifest || {}) };
    if (sc.no_remote) delete manifest.hashes;
    if (sc.pieces) {   // a disk of 24 pieces
      manifest.size = PIECE * sc.pieces; manifest.present = "ffffff"; manifest.hashes = hashes.slice(0, sc.pieces);
    }
    const memory = new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
    const control = new Int32Array(new SharedArrayBuffer(64));
    const events = [], logs = [], marks = {};
    const mk = () => { const c = new MessageChannel(); c.port1.on("message", (m) => events.push(m)); c.port1.unref(); return c; };
    const disk_port = mk(), fetcher_port = mk();
    const began_at = now();
    const h = {
      events, logs, marks, began_at, ready_at: 0, workers_started: false,
      sorted: () => events.slice().sort((a, b) => a.t - b.t),
      mark: (k) => { marks[k] = now(); },
      sleep: (ms) => new Promise((r) => setTimeout(r, ms)),
      until: async (pred, ms) => { const end = Date.now() + ms; while (!pred()) { if (Date.now() > end) throw new Error("timed out waiting"); await new Promise((r) => setTimeout(r, 10)); } },
      fetched: () => events.filter((e) => e.ev == "fetch" && e.phase == "start").sort((a, b) => a.t - b.t).map((e) => e.i),
      xhr: () => events.filter((e) => e.ev == "xhr" && e.phase == "start"),
      xhr_events: (piece, phase) => events.filter((e) => e.ev == "xhr" && e.piece == piece && e.phase == phase).sort((a, b) => a.t - b.t),
      site: () => events.filter((e) => e.ev == "xhr" && e.phase == "start" && !e.storage),
      requests_for: (i) => h.fetched().filter((x) => x == i).length + h.xhr().filter((e) => e.piece == i).length,
      lost_lines: () => logs.filter((l) => /removed this site's storage/.test(l)),
      requests_for_all_once: () => range(0, n).every((i) => h.requests_for(i) == 1),
      max_flying: () => Math.max(0, ...events.filter((e) => e.ev == "fetch" && e.phase == "start").map((e) => e.inflight)),
      max_total_flying_after: (t) => {   // pool requests plus the disk worker's own, at the most, from t on
        let flying = 0, worst = 0;
        for (const e of h.sorted()) {
          const d = (e.ev == "fetch" || e.ev == "xhr") ? (e.phase == "start" ? 1 : -1) : 0;
          flying += d;
          if (e.t >= t) worst = Math.max(worst, flying);
        }
        return worst;
      },
      read: async (i) => {   // as the kernel does: one request through the words, whole piece
        const length = pieces[i].length;
        control[3] = 0; control[4] = (i * PIECE) / 512; control[5] = 0;
        control[6] = 0; control[11] = 0; control[7] = Math.ceil(length / 512);
        Atomics.store(control, 2, 0); Atomics.store(control, 1, 1); Atomics.notify(control, 1);
        while (Atomics.load(control, 2) == 0) await Atomics.waitAsync(control, 2, 0, 1000).value;
        if (control[8] != 0) throw new Error("read of piece " + i + " failed: " + control[8]);
        if (!Buffer.from(new Uint8Array(memory.buffer, 0, length)).equals(pieces[i])) throw new Error("piece " + i + " differs");
      },
    };
    const w = new NodeWorker(__filename, { workerData: { role: "disk", name, port: disk_port.port2, fetcher_port: fetcher_port.port2, manifest, memory, control, hashes }, transferList: [disk_port.port2, fetcher_port.port2] });
    w.on("error", reject);
    w.on("message", async (m) => {
      if (m.method == "log") logs.push(m.text);
      else if (m.method == "failed") reject(new Error("worker failed: " + m.text));
      else if (m.method == "ready") {
        h.ready_at = now();
        try {
          await sc.drive(h);
          await h.sleep(60);   // (the last reports)
          let verdict = sc.check(h);
          if (verdict === true && sc.after) verdict = await sc.after(h);
          w.terminate();
          resolve({ verdict, h });
        } catch (e) { w.terminate(); reject(e); }
      }
    });
    w.on("message", (m) => { if (m.ev == "worker") h.workers_started = true; });
    disk_port.port1.on("message", (m) => { if (m.ev == "worker") h.workers_started = true; });
    setTimeout(() => reject(new Error("the scenario did not finish in 40 s")), 40000).unref();
  });

  (async () => {
    let failed = 0, ran = 0;
    for (const [name, sc] of Object.entries(scenarios)) {
      if (filter && !name.includes(filter)) continue;
      ran++;
      let line;
      try {
        const { verdict, h } = await run(name, sc);
        const good = verdict === true;
        line = (good ? "ok   " : "FAIL ") + name + (good ? "" : ": " + verdict)
          + (good ? "  [" + h.fetched().length + " fetched ahead, " + h.xhr().length + " own requests]" : "");
        if (!good) failed++;
        if (!good && process.env.POOL_DEBUG) {
          const t0 = h.began_at;
          for (const e of h.sorted()) console.log("   ", Math.round(e.t - t0), e.ev, e.phase || "", e.i ?? e.piece ?? "", e.inflight ?? "");
          console.log("    marks", JSON.stringify(Object.fromEntries(Object.entries(h.marks).map(([k, v]) => [k, Math.round(v - t0)]))), h.logs.join(" | "));
        }
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
