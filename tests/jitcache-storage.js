#!/usr/bin/env node
// tools/www/jitcache.js: what does not need a browser of the way the store is kept in a small or vanishing storage
// (budget(), changed_blocks(), a file cut short, a store cut down to less room). The browser side (the saving worker,
// the quota, the removal) is tests/storage-chrome.js.
//
//   node tests/jitcache-storage.js
"use strict";
const jc = require("../tools/www/jitcache.js");
const MiB = 1048576;
let failed = 0;
const check = (name, ok, detail) => { console.log((ok ? "ok   " : "FAIL ") + name + (ok ? "" : ": " + detail)); if (!ok) failed++; };

// budget: a share of the quota and of what is left of it; 0 below 16 MiB
check("roomy storage: what was asked for", jc.budget(384, { usage: 2000 * MiB, quota: 12240 * MiB }, 0) == 384, jc.budget(384, { usage: 2000 * MiB, quota: 12240 * MiB }, 0));
check("a quota of 1 GiB: 10% of it", jc.budget(384, { usage: 0, quota: 1024 * MiB }, 0) == 102, jc.budget(384, { usage: 0, quota: 1024 * MiB }, 0));
check("a quota of 1 GiB, 900 MiB in use: a quarter of what is left", jc.budget(384, { usage: 900 * MiB, quota: 1024 * MiB }, 0) == 31, jc.budget(384, { usage: 900 * MiB, quota: 1024 * MiB }, 0));
check("the file's own bytes are not counted against it", jc.budget(384, { usage: 900 * MiB, quota: 1024 * MiB }, 800 * MiB) > 31, "");
check("almost full: not kept (0)", jc.budget(384, { usage: 1000 * MiB, quota: 1024 * MiB }, 0) == 0, jc.budget(384, { usage: 1000 * MiB, quota: 1024 * MiB }, 0));
check("a quota of 100 MiB: not kept (10 MiB < 16)", jc.budget(384, { usage: 0, quota: 100 * MiB }, 0) == 0, "");
check("no estimate: what was asked for", jc.budget(384, null, 0) == 384, "");

// changed_blocks: only the 64 KiB blocks that differ
const a = new Uint8Array(300000), b = a.slice();
b[70000] = 1; b[299999] = 2;
const blocks = jc.changed_blocks(a, b);
check("changed_blocks: the two blocks that changed", blocks.length == 2 && blocks[0].at == 65536 && blocks[1].at == 262144 && blocks[1].data.length == 300000 - 262144, JSON.stringify(blocks.map((x) => [x.at, x.data.length])));
check("changed_blocks: everything without a before", jc.changed_blocks(null, b).length == 5, "");
check("changed_blocks: nothing if the same", jc.changed_blocks(b, b.slice()).length == 0, "");
check("changed_blocks: a longer after is written past the end", jc.changed_blocks(new Uint8Array(65536), new Uint8Array(70000)).length == 1, "");

// a store of 64 MiB with records, saved, read back with less room, and from a file whose last save was cut short
const store = jc.create(8, 1024);
const words = new Int32Array(store), bytes = new Uint8Array(store);
// (records the way rv/jitcache.c adds them is not needed: put a few by hand: key, offset, length, visit)
const data_at = 64 + 32 * 1024;
for (let r = 0; r < 20; r++) {
  const at = 64 + 32 * (r * 7);
  bytes.set(new Uint8Array(16).fill(r + 1), at);
  words[(at >> 2) + 4] = r * 100000;
  words[(at >> 2) + 5] = 100000;
  words[(at >> 2) + 6] = r;       // the visit it was last used in
  bytes.fill(r + 1, data_at + r * 100000, data_at + (r + 1) * 100000);
}
words[4] = 2000000; words[5] = 20; words[6] = 20; words[8] = 20;
const saved = jc.snapshot(store);
let back = jc.restore(saved, 8);
check("snapshot/restore round trip", back && jc.info(back).records == 20 && jc.info(back).used == 2000000, back && JSON.stringify(jc.info(back)));
const longer = new Uint8Array(saved.length + 5000);
longer.set(saved);
back = jc.restore(longer, 8);
check("a file longer than its index says (a save cut short) is still taken", back && jc.info(back).records == 20, "");
check("a file shorter than its index says is not", jc.restore(saved.subarray(0, saved.length - 1), 8) === null, "");
const smaller = jc.compact(jc.restore(saved, 8), 0.5, 1, true);   // 1 MiB of room, half of it kept
const si = jc.info(smaller);
check("cut down to less room: a new, smaller store with the records used most lately", si.capacity < 8 * MiB && si.capacity > 0 && si.records > 0 && si.records < 20 && si.used <= si.capacity * 0.5, JSON.stringify(si));
// the records kept are the later visits' ones (19, 18, ...)
const sw = new Int32Array(smaller);
const lasts = [];
for (let i = 0; i < si.places; i++) { if (sw[((64 + 32 * i) >> 2) + 5]) lasts.push(sw[((64 + 32 * i) >> 2) + 6]); }
check("the ones kept are the latest", Math.min(...lasts) >= 20 - si.records, lasts.join());

console.log(failed ? failed + " failed" : "all passed");
process.exit(failed ? 1 : 0);
