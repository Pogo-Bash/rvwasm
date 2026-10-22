#!/usr/bin/env node
// What Chrome does with the origin's private file system (OPFS) under a small quota and when its data is removed:
// the research behind tools/www/disk-worker.js and jitcache.js handling a storage that is small or goes away.
//
//   tools/with-browser.sh WHO node tools/storage-probe.js [--mode normal|context|guest] [--quota-mb N] [--watch SECONDS]
//
// In the page (a worker, as the disks' workers are) it prints: the quota and usage navigator.storage.estimate()
// reports; how far a FileSystemSyncAccessHandle can be written before it fails and with what; whether
// createWritable({keepExistingData: true}) copies the file (usage while a writable is open); and what an open sync
// access handle returns after the DevTools protocol removes the origin's file systems (Storage.clearDataForOrigin),
// which is what the browser's own removal of an origin's data looks like to a page.
"use strict";
const { spawn } = require("child_process");
const fs = require("fs");
const http = require("http");
const os = require("os");
const path = require("path");

if (process.env.RV_BROWSER != "1") {
  console.error("run this under tools/with-browser.sh (one test browser at a time)");
  process.exit(2);
}
const args = process.argv.slice(2);
const opt = (name, def) => (args.includes(name) ? args[args.indexOf(name) + 1] : def);
const mode = opt("--mode", "normal");
const quota_mb = Number(opt("--quota-mb", 0));
const watch_s = Number(opt("--watch", 0));
const CHROME = process.env.CHROME || path.join(__dirname,
  "chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const PAGE = `<!doctype html><meta charset=utf-8><title>probe</title><script>
const MiB = 1048576;
const worker_source = (${(() => {
  const MiB = 1048576;
  const say = (text) => postMessage({ say: text });
  onmessage = async (event) => {
    const m = event.data;
    const root = await navigator.storage.getDirectory();
    if (m.op == "fill") {
      // write 8 MiB at a time at the end of one file, until something fails
      const file = await (await root.getFileHandle("fill", { create: true })).createSyncAccessHandle();
      const chunk = new Uint8Array(8 * MiB).fill(7);
      let at = 0, error = null;
      try {
        for (;;) {
          const n = file.write(chunk, { at });
          if (n != chunk.length) { error = "short write " + n; break; }
          at += n;
          if (at > m.limit_mb * MiB) { break; }
        }
      } catch (e) { error = e.name + ": " + e.message; }
      say("fill: wrote " + (at / MiB) + " MiB, then " + (error || "stopped at the limit asked"));
      file.flush();
      say("fill: size now " + file.getSize() / MiB + " MiB");
      const est = await navigator.storage.estimate();
      say("fill: estimate usage " + Math.round(est.usage / MiB) + " MiB of quota " + Math.round(est.quota / MiB) + " MiB");
      try {
        const again = await root.getDirectoryHandle("nothing-here-yet", { create: true });
        say("fill: can still create a directory after the failure: " + again.name);
      } catch (e) { say("fill: cannot create a directory after the failure: " + e.name); }
      try { const probe = new Uint8Array(4096); say("fill: read at 0 returns " + file.read(probe, { at: 0 })); } catch (e) { say("fill: read threw " + e.name); }
      if (m.watch_s) {
        // does the browser take the data away by itself after a write failed for want of room? (an open handle: -4 when so)
        const t0 = Date.now();
        let gone_after = -1;
        while (Date.now() - t0 < m.watch_s * 1000) {
          await new Promise((r) => setTimeout(r, 1000));
          const probe = new Uint8Array(4096);
          let r;
          try { r = file.read(probe, { at: 0 }); } catch (e) { r = "threw " + e.name; }
          if (r !== 4096) { gone_after = Date.now() - t0; say("watch: read returned " + r + " after " + gone_after + " ms"); break; }
        }
        if (gone_after < 0) { say("watch: the data was still there after " + m.watch_s + " s"); }
      }
      file.close();
      await root.removeEntry("fill").catch((e) => say("fill: removeEntry " + e.name));
      await root.removeEntry("nothing-here-yet").catch(() => {});
      postMessage({ done: m.op });
    } else if (m.op == "swap") {
      // does a writable with keepExistingData copy the file? (usage while it is open)
      const f = await root.getFileHandle("swapfile", { create: true });
      const s = await f.createSyncAccessHandle();
      s.write(new Uint8Array(40 * MiB).fill(1), { at: 0 }); s.flush(); s.close();
      const before = (await navigator.storage.estimate()).usage;
      const w = await f.createWritable({ keepExistingData: true });
      await w.write({ type: "write", position: 0, data: new Uint8Array(4096).fill(2) });
      const during = (await navigator.storage.estimate()).usage;
      await w.close();
      const after = (await navigator.storage.estimate()).usage;
      say("swap: file 40 MiB; usage before " + Math.round(before / MiB) + ", while a writable (keepExistingData) is open " + Math.round(during / MiB) + ", after close " + Math.round(after / MiB) + " MiB");
      const w2 = await f.createWritable({ keepExistingData: false });
      await w2.write({ type: "write", position: 0, data: new Uint8Array(4096).fill(2) });
      const during2 = (await navigator.storage.estimate()).usage;
      await w2.abort();
      say("swap: while a writable without keepExistingData is open " + Math.round(during2 / MiB) + " MiB");
      await root.removeEntry("swapfile");
      postMessage({ done: m.op });
    } else if (m.op == "hold") {
      // a file and a directory, kept open as a disk is, for the page to remove the origin's data from under it
      const dir = await root.getDirectoryHandle("disk-probe", { create: true });
      const file = await (await dir.getFileHandle("data", { create: true })).createSyncAccessHandle();
      file.write(new Uint8Array(4096).fill(9), { at: 0 }); file.flush();
      self.held = { file, dir, root };
      postMessage({ done: m.op });
    } else if (m.op == "after") {
      const { file, dir, root } = self.held;
      const probe = new Uint8Array(4096);
      let r;
      try { r = file.read(probe, { at: 0 }); say("after removal: read returns " + r + (r > 2 ** 31 ? " (" + (r - 2 ** 32) + " as signed)" : "") + ", first byte " + probe[0]); } catch (e) { say("after removal: read threw " + e.name + ": " + e.message); }
      try { r = file.write(new Uint8Array(4096).fill(3), { at: 0 }); say("after removal: write returns " + r + (r > 2 ** 31 ? " (" + (r - 2 ** 32) + " as signed)" : "")); } catch (e) { say("after removal: write threw " + e.name + ": " + e.message); }
      try { say("after removal: getSize " + file.getSize()); } catch (e) { say("after removal: getSize threw " + e.name); }
      try { file.flush(); say("after removal: flush ok"); } catch (e) { say("after removal: flush threw " + e.name); }
      try { await root.getDirectoryHandle("disk-probe"); say("after removal: directory still there"); } catch (e) { say("after removal: getDirectoryHandle(no create) threw " + e.name + ": " + e.message); }
      try { await dir.getFileHandle("data2", { create: true }); say("after removal: create a file in the old directory handle ok"); } catch (e) { say("after removal: create a file in the old directory handle threw " + e.name + ": " + e.message); }
      try { await root.getFileHandle("fresh", { create: true }); say("after removal: create a file under the root ok"); } catch (e) { say("after removal: create a file under the root threw " + e.name); }
      postMessage({ done: m.op });
    }
  };
}).toString()})
const blob = new Blob(["(" + worker_source + ")()"], { type: "text/javascript" });
const w = new Worker(URL.createObjectURL(blob));
window.lines = [];
const waiting = {};
w.onmessage = (e) => { if (e.data.say) window.lines.push(e.data.say); if (e.data.done) { waiting[e.data.done](); } };
window.run = (op, extra) => new Promise((r) => { waiting[op] = r; w.postMessage(Object.assign({ op }, extra)); });
window.est = async () => { const e = await navigator.storage.estimate(); return { usage: e.usage, quota: e.quota }; };
</script>`;

const server = http.createServer((req, res) => {
  res.writeHead(200, { "Content-Type": "text/html" });
  res.end(PAGE);
});

function startChrome(profile) {
  const flags = ["--headless=new", "--user-data-dir=" + profile, "--remote-debugging-port=0", "--no-first-run",
    "--no-default-browser-check"];
  if (mode == "incognito") flags.push("--incognito");
  if (mode == "guest") flags.push("--guest");
  const chrome = spawn(CHROME, [...flags, "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
  return new Promise((resolve, reject) => {
    let err = "";
    chrome.stderr.on("data", (d) => { err += d; const m = err.match(/DevTools listening on (ws:\/\/\S+)/); if (m) resolve({ chrome, address: m[1] }); });
    chrome.on("exit", () => reject(new Error("Chrome exited: " + err)));
  });
}
function connect(address) {
  const ws = new WebSocket(address);
  const waiting = new Map();
  let next = 1;
  ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    if (msg.id && waiting.has(msg.id)) {
      const { resolve, reject } = waiting.get(msg.id);
      waiting.delete(msg.id);
      msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
    }
  };
  return new Promise((resolve, reject) => {
    ws.onerror = () => reject(new Error("cannot connect to Chrome"));
    ws.onopen = () => resolve({
      send(method, params = {}, sessionId) { const id = next++; ws.send(JSON.stringify({ id, method, params, sessionId })); return new Promise((res, rej) => waiting.set(id, { resolve: res, reject: rej })); },
      close: () => ws.close(),
    });
  });
}

async function main() {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const origin = "http://localhost:" + server.address().port;
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "rvdrive-storage-"));
  const { chrome, address } = await startChrome(profile);
  try {
    const cdp = await connect(address);
    // incognito: a browser context of its own, which is an off-the-record one (the quota and the file system of a private window)
    const context = mode == "context" ? (await cdp.send("Target.createBrowserContext", {})).browserContextId : undefined;
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank", browserContextId: context });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const evaluate = async (expression) => {
      const r = await cdp.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId);
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      return r.result.value;
    };
    await cdp.send("Page.navigate", { url: origin + "/" }, sessionId);
    await sleep(1500);
    const mib = (n) => Math.round(n / 1048576);
    const e0 = await evaluate("est()");
    console.log("mode " + mode + ": estimate quota " + mib(e0.quota) + " MiB, usage " + mib(e0.usage) + " MiB; userAgent " + (await evaluate("navigator.userAgent")).replace(/.*Chrome\//, "Chrome/"));
    console.log("persist(): " + await evaluate("navigator.storage.persist()") + "; persisted(): " + await evaluate("navigator.storage.persisted()"));
    const lines = async () => { const l = await evaluate("window.lines.splice(0)"); for (const x of l) console.log("  " + x); };
    const storage = (method, params) => cdp.send(method, Object.assign({}, params, context ? {} : {}), sessionId)
      .catch((e) => { console.log(method + " on the page: " + e.message); return cdp.send(method, params); });
    if (quota_mb) {
      await storage("Storage.overrideQuotaForOrigin", { origin, quotaSize: quota_mb * 1048576 });
      const e1 = await evaluate("est()");
      console.log("after Storage.overrideQuotaForOrigin(" + quota_mb + " MiB): estimate quota " + mib(e1.quota) + " MiB, usage " + mib(e1.usage));
    }
    const limit = quota_mb ? quota_mb * 2 : Math.min(mib(e0.quota) + 64, 3000);
    await evaluate("run('fill', { limit_mb: " + limit + ", watch_s: " + watch_s + " })"); await lines();
    await evaluate("run('swap')"); await lines();
    await evaluate("run('hold')");
    await storage("Storage.clearDataForOrigin", { origin, storageTypes: "file_systems" });
    console.log("Storage.clearDataForOrigin(file_systems) sent");
    await evaluate("run('after')"); await lines();
    cdp.close();
  } finally {
    chrome.kill();
    server.close();
    await sleep(1500);
    try { fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 500 }); } catch (e) { /* (left) */ }
  }
}
main().catch((e) => { console.error(e); process.exit(1); });
