#!/usr/bin/env node
// The page's background fetch of the native programs' pieces (tools/www/dev.html, between BEGIN disk-warm and END disk-warm) against a mock:
// what it fetches (a manifest's warm list, else every piece of the nqs disk), the disks taken in turn, two at a time, only once the desktop
// is up and quiet, nothing while a program is starting, nothing on a slow or data-saving connection, nothing once done. Needs only node.
const fs = require("fs");
const html = fs.readFileSync(__dirname + "/../../tools/www/dev.html", "utf8");
const text = html.slice(html.indexOf("// BEGIN disk-warm"), html.indexOf("// END disk-warm"));
const { warmDisks } = new Function(text + "\nreturn { warmDisks };")();
let failed = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) failed++; };
const manifests = {
  "disk/release/manifest.json": { id: "r1", base: "/disk/", hashes: ["h0", "h1", "h2", "h3", "h4"], present: "ff", warm: [3, 1] },
  "disk/nqs/manifest.json": { id: "n1", base: "https://b.example/disk", hashes: ["n0", "n1", "", "n3"], present: "d" },   // present: 1101 -> pieces 0 1 3 (2 has no hash)
};
const run = async (extra = {}) => {
  let clock = 0, windows = 0, flying = 0, maxFlying = 0, launching = false;
  const fetched = [], done = new Set(extra.done || []), logs = [];
  const o = {
    base: "http://x.example/desktop",
    disks: [{ name: "release", url: "disk/release/" }, { name: "nqs", url: "disk/nqs/", all: true }],
    fetchJson: async (u) => manifests[u] || null,
    fetchPiece: async (u) => { flying++; maxFlying = Math.max(maxFlying, flying); fetched.push([clock, u]); clock += 500; flying--; return 1048576; },
    sleep: async (ms) => { clock += ms; if (clock >= 8000) windows = 2; if (clock >= 30000 && clock < 33000) launching = true; else launching = false; },
    now: () => clock,
    display: () => ({ windows }),
    launching: () => launching,
    connection: () => extra.connection || null,
    done: (k) => done.has(k), setDone: (k) => done.add(k), log: (m) => logs.push(m),
  };
  const r = await warmDisks(o);
  return { r, fetched, done, logs, maxFlying };
};
(async () => {
  let x = await run();
  const urls = x.fetched.map((f) => f[1]);
  check(urls.join(" ") === "http://x.example/disk/h3.bin https://b.example/disk/n0.bin http://x.example/disk/h1.bin https://b.example/disk/n1.bin https://b.example/disk/n3.bin",
        "warm list of release, all pieces of nqs, taken in turn: " + urls.join(" "));
  check(x.fetched[0][0] >= 8000 + 5000 - 1000, "nothing before the desktop is up and quiet (first at " + x.fetched[0][0] + " ms)");
  check(x.done.size === 1, "marked done after a complete run");
  x = await run({ done: ["rv-warm-release:r1,nqs:n1"] });
  check(x.fetched.length === 0 && x.r.skipped === "done before", "nothing once done");
  x = await run({ connection: { saveData: true } });
  check(x.fetched.length === 0 && x.r.skipped === "connection", "nothing with data saver");
  x = await run({ connection: { effectiveType: "3g" } });
  check(x.fetched.length === 0, "nothing on 3g");
  x = await run({ connection: { effectiveType: "4g" } });
  check(x.fetched.length === 5, "4g is fine");
  process.exit(failed ? 1 : 0);
})();
