// The page's side of the store of translated code (tools/www/jitcache.js), under Node, with an origin private file
// system kept in memory: page loads one after another, each adding records the way the runtime's __rv_jit_cache
// does (tools/patch-runtime.py, 18), saving, and the next load finding them. Also: a store of another build of the
// emulator is removed, ?fresh-jitcache forgets, a store more than 3/4 full keeps what was used last.
//   node tests/jitcache/store.js        (prints "ok" and exits 0, or says what failed and exits 1)
"use strict";
const path = require("path");
const jc = require(path.join(__dirname, "..", "..", "tools", "www", "jitcache.js"));

// An origin private file system in memory: name -> Uint8Array.
const files = new Map();
function directory() {
  return {
    async *entries() {
      for (const name of [...files.keys()]) yield [name, {}];
    },
    async removeEntry(name) {
      files.delete(name);
    },
    async getFileHandle(name, opts) {
      if (typeof name != "string") throw new TypeError("file name " + name);
      if (!files.has(name)) {
        if (!(opts && opts.create)) throw Object.assign(new Error("no " + name), { name: "NotFoundError" });
        files.set(name, new Uint8Array(0));
      }
      return {
        async getFile() {
          const b = files.get(name);
          return { async arrayBuffer() { return b.slice().buffer; } };
        },
        async createWritable(o) {
          let b = o && o.keepExistingData ? files.get(name).slice() : new Uint8Array(0);
          return {
            async write(op) {
              const end = op.position + op.data.length;
              if (end > b.length) { const n = new Uint8Array(end); n.set(b); b = n; }
              b.set(op.data, op.position);
            },
            async truncate(n) { const t = new Uint8Array(n); t.set(b.subarray(0, n)); b = t; },
            async close() { files.set(name, b); },
          };
        },
      };
    },
  };
}
Object.defineProperty(globalThis, "navigator", { value: { storage: { getDirectory: async () => directory() } } });
globalThis.document = { addEventListener() {}, visibilityState: "visible" };
const timers = [];
const realSetInterval = setInterval;
globalThis.setInterval = (f) => { timers.push(f); return 0; };

// What the runtime's hook does (op 1 keeps, op 0 finds), with a key whose first word is k.
function put(buf, k, len, fill) {
  const w = new Int32Array(buf), b = new Uint8Array(buf), places = w[2], data = 64 + 32 * places;
  if (2 * (w[5] + 1) > places || w[4] + len > w[3]) return -1;
  let i = k & (places - 1);
  for (;; i = (i + 1) & (places - 1)) {
    if (!w[((64 + 32 * i) >> 2) + 5]) break;
    if (w[(64 + 32 * i) >> 2] == k) return -1;
  }
  const at = 64 + 32 * i;
  w[at >> 2] = k;
  b.fill(fill, data + w[4], data + w[4] + len);
  w[(at >> 2) + 4] = w[4]; w[(at >> 2) + 6] = w[8]; w[(at >> 2) + 5] = len;
  w[4] += (len + 7) & ~7; w[5]++; w[6]++;
  return 0;
}
function get(buf, k) {
  const w = new Int32Array(buf), b = new Uint8Array(buf), places = w[2], data = 64 + 32 * places;
  for (let i = k & (places - 1);; i = (i + 1) & (places - 1)) {
    const at = 64 + 32 * i;
    if (!w[(at >> 2) + 5]) return null;
    if (w[at >> 2] == k) {
      w[(at >> 2) + 6] = w[8]; w[9]++;
      const o = data + w[(at >> 2) + 4], n = w[(at >> 2) + 5];
      for (let j = 0; j < n; j++) if (b[o + j] != b[o]) return "torn";
      return b[o];
    }
  }
}

let failed = 0;
function check(what, ok) {
  console.log((ok ? "ok   " : "FAIL ") + what);
  if (!ok) failed++;
}
const lines = [];
const log = (l) => lines.push(l);
const A = "a".repeat(64), B = "b".repeat(64);
const load = async (o) => { timers.length = 0; lines.length = 0; return jc.open(Object.assign({ megabytes: 1, log }, o)); };
const save = async () => { for (const f of timers) await f(); };

(async () => {
  // Load 1: empty; 100 records; saved.
  let buf = await load({ program: A });
  check("load 1 starts empty", jc.info(buf).records == 0 && lines.some((l) => /empty/.test(l)));
  for (let k = 1; k <= 100; k++) put(buf, k, 3000, k & 255);
  await save();
  check("load 1 saved under the build's name", files.has("jitcache-" + A.slice(0, 16) + ".bin") && files.size == 1);
  // Load 2: kept; all 100 there; 50 more added; saved again (only what was added is written).
  buf = await load({ program: A });
  check("load 2 is the kept store", lines.some((l) => /kept, 100 records/.test(l)));
  check("load 2 finds load 1's records", [...Array(100)].every((_, i) => get(buf, i + 1) === ((i + 1) & 255)));
  for (let k = 101; k <= 150; k++) put(buf, k, 3000, k & 255);
  await save();
  buf = await load({ program: A });
  check("load 3 finds both loads' records", jc.info(buf).records == 150 &&
        [...Array(150)].every((_, i) => get(buf, i + 1) === ((i + 1) & 255)));
  // Nothing new and nothing found: no save.
  const before = files.get("jitcache-" + A.slice(0, 16) + ".bin");
  buf = await load({ program: A });
  await save();
  check("a load that neither adds nor finds does not write", files.get("jitcache-" + A.slice(0, 16) + ".bin") === before);
  // Fill past 3/4 (150 * 3000 + more of 1 MiB); use 1..40 in this load; the next load keeps those and the new ones.
  for (let k = 1; k <= 40; k++) get(buf, k);
  for (let k = 200; k < 320; k++) put(buf, k, 3000, k & 255);  // (270 records of 3000 bytes: 77%)
  await save();
  buf = await load({ program: A });
  const kept = (k) => get(buf, k) === (k & 255);
  check("a full store is cut down at the next load (" + lines[0] + ")", /forgotten/.test(lines.join("\n")) &&
        jc.info(buf).used <= 0.5 * jc.info(buf).capacity);
  check("what was used last load is kept", [...Array(40)].every((_, i) => kept(i + 1)));
  check("what was not used is forgotten first", !kept(120));
  await save();
  buf = await load({ program: A });
  check("the store cut down is saved whole and loads", [...Array(40)].every((_, i) => kept(i + 1)));
  // Another build of the emulator: the old store is removed, the new one starts empty.
  buf = await load({ program: B });
  check("another build starts empty and removes the old store", jc.info(buf).records == 0 &&
        !files.has("jitcache-" + A.slice(0, 16) + ".bin"));
  put(buf, 7, 100, 7);
  await save();
  buf = await load({ program: B, reset: true });
  check("?fresh-jitcache forgets", jc.info(buf).records == 0 && lines.some((l) => /removed/.test(l)));
  check("no stray files", [...files.keys()].every((n) => /^jitcache-[0-9a-f]{16}\.bin$/.test(n)));
  console.log(failed ? failed + " failed" : "ok");
  process.exit(failed ? 1 : 0);
})();
