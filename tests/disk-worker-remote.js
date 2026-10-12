#!/usr/bin/env node
// tools/www/disk-worker.js fetching pieces from object storage (the manifest's `base` and `hashes`) and falling back
// to the site's own copy. No browser: the worker's source runs in a Node worker thread with stand-ins for
// XMLHttpRequest and fetch, serving from memory according to a scenario; every URL it asks for is reported. The
// disk is read through the worker's request words, as the kernel does. The worker's waits (Atomics.wait on a private
// word) are not slept in the test: they are recorded and added to a virtual clock, so a 30 s Retry-After or the
// two-minute limit costs nothing and is still checked.
//
//   node tests/disk-worker-remote.js
"use strict";
const { Worker, isMainThread, parentPort, workerData } = require("node:worker_threads");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const vm = require("node:vm");

const PIECE = 4096, PIECES = 8, SIZE = PIECE * (PIECES - 1) + 1024;  // the last piece is short
const BASE = "disk/t/", STORE = "https://disk.test/disk/", ORIGIN = "https://site.test";

const ok = (bytes) => ({ status: 200, body: bytes });
const mine = (url, pieces) => {  // the site's own copy: BASE + number
  const n = Number(url.slice(url.lastIndexOf("/") + 1));
  return n >= 0 && n < PIECES ? ok(pieces[n]) : { status: 404 };
};

// name -> { manifest tweaks, a factory for the function that answers a request, what must have happened }
const scenarios = {
  "all from storage": {
    base: STORE, serve: ({ stored }) => (url, p) => stored(url, STORE) || { status: 500 },
    expect: { storage: 8, site: 0 } },
  "storage 403 (CORS refused): the site's copy": {
    base: STORE, serve: () => (url, p) => url.startsWith(STORE) ? { status: 403 } : mine(url, p),
    expect: { storage: 3, site: 8, log: /failed three times/ } },
  "storage unreachable: the site's copy": {
    base: STORE, serve: () => (url, p) => url.startsWith(STORE) ? { throws: true } : mine(url, p),
    expect: { storage: 3, site: 8, log: /failed three times/ } },
  "storage answers 200 with a wrong size": {
    base: STORE, serve: () => (url, p) => url.startsWith(STORE) ? ok(Buffer.alloc(77)) : mine(url, p),
    expect: { storage: 3, site: 8 } },
  "storage fails once, then works": {
    base: STORE, serve: ({ stored }) => { let first = true; return (url, p) => url.startsWith(STORE) ? (first ? (first = false, { status: 503 }) : stored(url, STORE)) : mine(url, p); },
    expect: { storage: 8, site: 1 } },
  "no base in the manifest": {
    base: undefined, serve: () => mine, expect: { storage: 0, site: 8 } },
  "base without hashes": {
    base: STORE, nohashes: true, serve: () => mine, expect: { storage: 0, site: 8 } },
  "a base relative to the site": {
    base: "/store", serve: ({ stored }) => (url, p) => stored(url, ORIGIN + "/store/") || { status: 500 },
    expect: { storage: 8, site: 0 } },

  // ---- the site's own copy under load (no object storage in these) ----
  "site 429 with Retry-After 2: waits as asked, then works": {
    base: undefined, serve: () => { let n = 0; return (url, p) => ++n == 1 ? { status: 429, headers: { "retry-after": "2" } } : mine(url, p); },
    expect: { storage: 0, site: 8 },
    check: (r) => r.sleeps.length == 1 && r.sleeps[0] >= 2000 && r.sleeps[0] <= 2400 || "sleeps " + r.sleeps },
  "site 429 with Retry-After 120: capped at 30 s": {
    base: undefined, serve: () => { let n = 0; return (url, p) => ++n == 1 ? { status: 429, headers: { "retry-after": "120" } } : mine(url, p); },
    expect: { storage: 0, site: 8 },
    check: (r) => r.sleeps.length == 1 && r.sleeps[0] == 30000 || "sleeps " + r.sleeps },
  "site 503 with a Retry-After date": {
    base: undefined, serve: () => { let n = 0; return (url, p) => ++n == 1 ? { status: 503, headers: { "retry-after": new Date(Date.now() + 5000).toUTCString() } } : mine(url, p); },
    expect: { storage: 0, site: 8 },
    check: (r) => r.sleeps.length == 1 && r.sleeps[0] >= 3000 && r.sleeps[0] <= 6100 || "sleeps " + r.sleeps },
  "site 429 with Retry-After 0: still a pause": {
    base: undefined, serve: () => { let n = 0; return (url, p) => ++n == 1 ? { status: 429, headers: { "retry-after": "0" } } : mine(url, p); },
    expect: { storage: 0, site: 8 },
    check: (r) => r.sleeps.length == 1 && r.sleeps[0] >= 250 || "sleeps " + r.sleeps },
  "site 500 five times: backoff doubles with jitter": {
    base: undefined, serve: () => { let n = 0; return (url, p) => ++n <= 5 ? { status: 500 } : mine(url, p); },
    expect: { storage: 0, site: 8 },
    check: (r) => r.sleeps.length == 5 && r.sleeps.every((w, i) => w >= 250 * 2 ** i && w <= 500 * 2 ** i) || "sleeps " + r.sleeps },
  "site network errors: backoff too": {
    base: undefined, serve: () => { let n = 0; return (url, p) => ++n <= 3 ? { throws: true } : mine(url, p); },
    expect: { storage: 0, site: 8 },
    check: (r) => r.sleeps.length == 3 && r.sleeps.every((w, i) => w >= 250 * 2 ** i && w <= 500 * 2 ** i) || "sleeps " + r.sleeps },
  "site 503 for ever: gives up (EIO) after about two minutes, without hammering": {
    base: undefined, serve: () => (url, p) => ({ status: 503 }), fails: true,
    expect: { storage: 0, site: 1 },
    check: (r) => {
      const total = r.sleeps.reduce((a, b) => a + b, 0);
      const worst = Math.max(...r.sleeps);
      return (total <= 120000 && total >= 60000 && worst <= 30000 && r.hits.length <= 20 && r.logs.some((l) => /error: cannot fetch .*503/.test(l)))
        || "slept " + total + " ms in " + r.sleeps.length + " waits, " + r.hits.length + " requests"; } },
  "site 404: two tries, no long waits": {
    base: undefined, serve: () => (url, p) => ({ status: 404 }), fails: true,
    expect: { storage: 0, site: 2 },
    check: (r) => r.hits.length == 2 && r.sleeps.length == 1 && r.sleeps[0] <= 500 || "hits " + r.hits.length + " sleeps " + r.sleeps },
  "storage 429 with Retry-After: the site serves meanwhile, the storage is not asked again": {
    base: STORE, serve: ({ stored }) => { let n = 0; return (url, p) => url.startsWith(STORE) ? (++n == 1 ? { status: 429, headers: { "retry-after": "20" } } : stored(url, STORE)) : mine(url, p); },
    expect: { storage: 1, site: 8 },
    check: (r) => r.sleeps.length == 0 || "sleeps " + r.sleeps },
};

if (!isMainThread) {
  // ---- the worker thread: the disk worker's source, with stand-ins ----
  const { name, pieces, manifest, memory, control } = workerData;
  const byHash = new Map(manifest.hashes ? manifest.hashes.map((h, i) => [h, i]) : []);
  const stored = (url, prefix) => url.startsWith(prefix) ? ok(pieces[byHash.get(url.slice(prefix.length, -4))]) : null;
  const answer = scenarios[name].serve({ stored });
  // The clock and the waits: Atomics.wait on anything but the request words is recorded and added to the clock.
  let skew = 0;
  const realNow = Date.now.bind(Date), realWait = Atomics.wait.bind(Atomics);
  Date.now = () => realNow() + skew;
  Atomics.wait = (array, index, value, timeout) => {
    if (array.buffer === control.buffer) return realWait(array, index, value, timeout);
    parentPort.postMessage({ method: "sleep", ms: timeout });
    skew += timeout;
    return "timed-out";
  };
  globalThis.self = globalThis;
  globalThis.location = new URL(ORIGIN + "/disk-worker.js");
  globalThis.onmessage = null;
  globalThis.postMessage = (m) => parentPort.postMessage(m);
  globalThis.fetch = async () => ({ json: async () => manifest });
  globalThis.XMLHttpRequest = class {
    open(method, url, async_) { this.url = url; if (async_ !== false) throw new Error("the worker must use synchronous requests"); }
    getResponseHeader(name) { return this.headers[name.toLowerCase()] ?? null; }
    send() {
      parentPort.postMessage({ method: "hit", url: this.url });
      const r = answer(this.url, pieces);
      if (r.throws) throw new Error("NetworkError");
      this.status = r.status;
      this.headers = r.headers || {};
      this.response = r.body ? r.body.buffer.slice(r.body.byteOffset, r.body.byteOffset + r.body.byteLength) : new ArrayBuffer(0);
    }
  };
  vm.runInThisContext(fs.readFileSync(path.join(__dirname, "../tools/www/disk-worker.js"), "utf8"));
  globalThis.onmessage({ data: { memory, control, url: BASE, name: "t", keep: false, reset: true } });
} else {
  const pieces = [], hashes = [];
  for (let i = 0; i < PIECES; i++) {
    pieces.push(crypto.randomBytes(i == PIECES - 1 ? SIZE - PIECE * i : PIECE));
    hashes.push(crypto.createHash("sha256").update(pieces[i]).digest("hex"));
  }

  const run = (name, s) => new Promise((resolve, reject) => {
    const manifest = { size: SIZE, piece: PIECE, id: "t", present: "ff", hashes: s.nohashes ? undefined : hashes, base: s.base };
    const memory = new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
    const control = new Int32Array(new SharedArrayBuffer(64));
    const w = new Worker(__filename, { workerData: { name, pieces, manifest, memory, control } });
    w.on("error", reject);
    const hits = [], logs = [], sleeps = [];
    w.on("message", (m) => {
      if (m.method == "hit") hits.push(m.url);
      else if (m.method == "sleep") sleeps.push(m.ms);
      else if (m.method == "log") logs.push(m.text);
      else if (m.method == "failed") reject(new Error("worker failed: " + m.text));
      else if (m.method == "ready") {
        (async () => {
          for (let i = 0; i < (s.fails ? 1 : PIECES); i++) {  // read every piece, whole, through the request words
            const length = pieces[i].length;
            control[3] = 0;                                         // OP_READ
            control[4] = (i * PIECE) / 512; control[5] = 0;         // sector
            control[6] = 0; control[11] = 0; control[7] = Math.ceil(length / 512);
            Atomics.store(control, 2, 0);
            Atomics.store(control, 1, 1);
            Atomics.notify(control, 1);
            while (Atomics.load(control, 2) == 0) await Atomics.waitAsync(control, 2, 0, 1000).value;
            if (s.fails) {
              if (control[8] != -5) throw new Error("the read should have failed with EIO, gave " + control[8]);
              continue;
            }
            if (control[8] != 0) throw new Error("read of piece " + i + " failed: " + control[8]);
            if (!Buffer.from(new Uint8Array(memory.buffer, 0, length)).equals(pieces[i])) throw new Error("piece " + i + " differs");
          }
          await new Promise((r) => setTimeout(r, 100));  // (the worker's last reports)
        })().then(() => { w.terminate(); resolve({ hits, logs, sleeps }); }, (e) => { w.terminate(); reject(e); });
      }
    });
  });

  (async () => {
    let failed = 0;
    for (const [name, s] of Object.entries(scenarios)) {
      let line;
      try {
        const r = await run(name, s);
        const { hits, logs } = r;
        const site = hits.filter((u) => u.startsWith(BASE)).length;
        const storage = hits.length - site;
        const e = s.expect;
        const verdict = s.check ? s.check(r) : true;
        const good = storage == e.storage && site >= e.site && (!e.log || logs.some((l) => e.log.test(l))) && verdict === true;
        line = (good ? "ok   " : "FAIL ") + name + ": " + storage + " requests to the storage, " + site + " to the site"
          + (good ? "" : " (expected " + JSON.stringify(e, (k, v) => v instanceof RegExp ? String(v) : v)
            + (typeof verdict == "string" ? "; " + verdict : "") + ")");
        if (!good) failed++;
      } catch (e) {
        line = "FAIL " + name + ": " + e.message;
        failed++;
      }
      console.log(line);
    }
    process.exit(failed ? 1 : 0);
  })();
}
