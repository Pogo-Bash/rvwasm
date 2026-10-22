#!/usr/bin/env node
// tools/www/jitcache.js (the store of translated code) in a real Chrome, against a small and a vanishing storage: its saving
// worker (in-place, incremental writes), the quota (QuotaExceededError / no room), the removal of the origin's data
// (NotFoundError), the budget by navigator.storage.estimate(). A page of its own loads jitcache.js and fills the store with
// records; the browser is Chrome for Testing, headless, in a browser context of its own with --private (an off-the-record
// one, like a private or Guest window: its file system is in memory, which is what the owner's failure was).
//
//   tools/with-browser.sh WHO node tests/storage-chrome.js [--private]
//
// How the storage is limited and taken away: Storage.overrideQuotaForOrigin (writes fail past the cap: a short write of
// 4294967288 in a private context, a QuotaExceededError in the default one; navigator.storage.estimate() does NOT show
// the cap) and Storage.clearDataForOrigin with "file_systems" (an open handle then answers 4294967292, see tools/storage-probe.js).
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
const private_context = args.includes("--private");
const www = path.join(__dirname, "../tools/www");
const CHROME = process.env.CHROME || path.join(__dirname, "../tools/chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const MiB = 1048576;

const PAGE = `<!doctype html><meta charset=utf-8><title>jitcache storage</title>
<script src="jitcache.js"></script>
<script>
window.lines = [];
const log = (t) => window.lines.push(t);
const PROGRAM = "ab".repeat(32);
// records as rv/jitcache.c adds them: key (16 bytes), offset, length, last visit; the store's counters
window.add_records = (buffer, n, kb, visit) => {
  const words = new Int32Array(buffer), bytes = new Uint8Array(buffer);
  const places = words[2], data = 64 + 32 * places;
  for (let r = 0; r < n; r++) {
    const used = words[4], length = kb * 1024;
    const key = new Uint8Array(16);
    new DataView(key.buffer).setUint32(0, Math.floor(Math.random() * 4e9), true);
    crypto.getRandomValues(key.subarray(4));   // (key is a plain buffer)
    let place = new DataView(key.buffer).getUint32(0, true) & (places - 1);
    while (words[((64 + 32 * place) >> 2) + 5]) place = (place + 1) & (places - 1);
    const at = 64 + 32 * place;
    bytes.set(key, at);
    words[(at >> 2) + 4] = used; words[(at >> 2) + 5] = length; words[(at >> 2) + 6] = visit;
    const noise = new Uint8Array(Math.min(length, 65536));
    crypto.getRandomValues(noise);   // (not into the shared buffer: not allowed)
    bytes.set(noise, data + used);
    words[4] = used + length; words[5]++; words[6]++;
  }
};
window.open_store = async (megabytes) => { window.store = await jitcache.open({ megabytes, program: PROGRAM, log }); return jitcache.info(window.store); };
window.file_size = async () => { try { return (await (await (await navigator.storage.getDirectory()).getFileHandle("jitcache-" + PROGRAM.slice(0, 16) + ".bin")).getFile()).size; } catch (e) { return e.name; } };
window.usage = async () => (await navigator.storage.estimate()).usage;
window.take_lines = () => window.lines.splice(0);
window.fake_estimate = (usage, quota) => { navigator.storage.estimate = async () => ({ usage, quota }); };
</script>`;

const server = http.createServer((req, res) => {
  const p = decodeURIComponent(req.url.split("?")[0]);
  const headers = { "Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp", "Cache-Control": "no-store" };
  if (p == "/") {
    res.writeHead(200, { ...headers, "Content-Type": "text/html" });
    res.end(PAGE);
  } else if (p == "/jitcache.js") {
    res.writeHead(200, { ...headers, "Content-Type": "text/javascript" });
    res.end(fs.readFileSync(path.join(www, "jitcache.js")));
  } else {
    res.writeHead(404, headers);
    res.end();
  }
});

function startChrome(profile) {
  const chrome = spawn(CHROME, ["--headless=new", "--user-data-dir=" + profile, "--remote-debugging-port=0", "--no-first-run",
    "--no-default-browser-check", "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
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

let failed = 0;
const check = (name, ok, detail) => { console.log((ok ? "ok   " : "FAIL ") + name + (ok ? "" : ": " + detail)); if (!ok) failed++; };

async function main() {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const origin = "http://localhost:" + server.address().port;
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "rvdrive-storagejc-"));
  const { chrome, address } = await startChrome(profile);
  try {
    const cdp = await connect(address);
    const context = private_context ? (await cdp.send("Target.createBrowserContext", {})).browserContextId : undefined;
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank", browserContextId: context });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    await cdp.send("Page.enable", {}, sessionId);
    const evaluate = async (expression) => {
      const r = await cdp.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId);
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      return r.result.value;
    };
    const load = async () => { await cdp.send("Page.navigate", { url: origin + "/" }, sessionId); await sleep(800); };
    const lines = async () => evaluate("take_lines()");
    console.log((private_context ? "private (off-the-record) browser context" : "default profile") + ", " + origin);

    // ---- A: a fresh store, saved ----
    await load();
    let info = await evaluate("open_store(32)");
    check("A open: empty store, 32 MiB of room", info.records == 0 && info.capacity == 32 * MiB, JSON.stringify(info));
    await evaluate("add_records(store, 30, 200, 1)");
    await evaluate("jitcache.save_now()");
    let l = await lines();
    check("A save: one 'saved' line", l.filter((x) => /saved, 30 records/.test(x)).length == 1, l.join(" | "));
    const places = info.places;
    const size_a = await evaluate("file_size()");
    const expect_a = 64 + 32 * places + 30 * 200 * 1024;
    check("A file: head + index + the 30 records", size_a == expect_a, size_a + " vs " + expect_a);

    // ---- B: the next visit; an incremental save writes only what is new, and never copies the file ----
    await load();
    info = await evaluate("open_store(32)");
    l = await lines();
    check("B open: the 30 records kept", info.records == 30 && l.some((x) => /kept, 30 records/.test(x)), JSON.stringify(info) + " " + l.join(" | "));
    await evaluate("add_records(store, 5, 200, 2)");
    // usage while saving: sampled every 2 ms; a copy of the 6 MB file would show as +6 MB
    const usage0 = await evaluate("usage()");
    await evaluate("window.peak = 0; window.sampler = setInterval(async () => { window.peak = Math.max(window.peak, await usage()); }, 2)");
    await evaluate("jitcache.save_now()");
    await sleep(100);
    await evaluate("clearInterval(window.sampler)");
    const peak = await evaluate("window.peak"), usage1 = await evaluate("usage()");
    l = await lines();
    const written = /\(([\d.]+) MiB written\)/.exec(l.join(" "));
    check("B save: only the new records (and the changed index blocks) written", written && Number(written[1]) < 3 && Number(written[1]) >= 1, l.join(" | "));
    check("B save: no copy of the file while it is written (peak usage no more than the final)", peak <= usage1 + 0.5 * MiB, "usage " + usage0 + " -> peak " + peak + " -> " + usage1);
    const size_b = await evaluate("file_size()");
    check("B file: grew by the new records", size_b == expect_a + 5 * 200 * 1024, size_b + " vs " + (expect_a + 5 * 200 * 1024));

    // ---- C: the budget by the estimate (the page says what it is told) ----
    await load();
    await evaluate("fake_estimate(0, 1024 * 1048576)");
    info = await evaluate("open_store(384)");
    l = await lines();
    check("C estimate quota 1 GiB: room for 102 MiB, by the log's word", info.capacity == 102 * MiB && l.some((x) => /room for 102 MiB of the 384/.test(x)), JSON.stringify(info) + l.join(" | "));
    await load();
    await evaluate("fake_estimate(990 * 1048576, 1024 * 1048576)");
    info = await evaluate("open_store(384)");
    l = await lines();
    check("C estimate nearly full: not kept in the storage, 128 MiB in memory only", info.capacity == 128 * MiB && l.some((x) => /storage is small/.test(x)), JSON.stringify(info) + l.join(" | "));
    check("C ... and it starts no saving", (await evaluate("typeof jitcache.save_now")) == "undefined", "");
    check("C ... the file from before is left as it was", (await evaluate("file_size()")) == size_b, String(await evaluate("file_size()")));

    // ---- D: the quota runs out (the cap is a little above what is there) ----
    await load();
    info = await evaluate("open_store(32)");
    await evaluate("take_lines()");
    const now_usage = await evaluate("usage()");
    await cdp.send("Storage.overrideQuotaForOrigin", { origin, quotaSize: now_usage + 2 * MiB }, sessionId);
    await evaluate("add_records(store, 20, 200, 3)");   // 4 MB
    await evaluate("jitcache.save_now()");
    l = await lines();
    const quiet = l.filter((x) => /not kept in the browser's storage any more/.test(x));
    check("D quota: one quiet line", quiet.length == 1 && /Quota|room/i.test(quiet[0]), l.join(" | "));
    check("D quota: the file is removed (the disks come first)", (await evaluate("file_size()")) == "NotFoundError", String(await evaluate("file_size()")));
    check("D quota: the store goes on in memory", (await evaluate("jitcache.info(store).records")) == 30 + 5 + 20, String(await evaluate("jitcache.info(store).records")));
    await evaluate("jitcache.save_now()");
    check("D quota: no more lines on the next save", (await lines()).length == 0, "");
    await cdp.send("Storage.overrideQuotaForOrigin", { origin }, sessionId);   // (the cap off)

    // ---- E: the browser takes the storage away in the middle of a visit ----
    await load();
    info = await evaluate("open_store(32)");
    await evaluate("add_records(store, 10, 200, 1); jitcache.save_now()");
    l = await lines();
    check("E before: saved", l.some((x) => /saved, 10 records/.test(x)), l.join(" | "));
    await cdp.send("Storage.clearDataForOrigin", { origin, storageTypes: "file_systems" }, sessionId);
    await evaluate("add_records(store, 10, 200, 1)");
    await evaluate("jitcache.save_now()");
    l = await lines();
    const gone = l.filter((x) => /not kept in the browser's storage any more/.test(x));
    // (A disk-backed profile keeps the unlinked file open: a save goes on writing to it. The private context's file system is gone.)
    if (private_context) {
      check("E removal: one quiet line (NotFoundError)", gone.length == 1 && /NotFoundError|removed/.test(gone[0]), l.join(" | "));
      await evaluate("add_records(store, 2, 200, 1); jitcache.save_now()");
      check("E removal: nothing more after it", (await lines()).length == 0, "");
    } else {
      console.log("     (default profile: the handle outlives the removal: " + (l.join(" | ") || "no line") + ")");
    }
    cdp.close();
  } finally {
    chrome.kill();
    server.close();
    await sleep(1500);
    try { fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 500 }); } catch (e) { /* (left) */ }
  }
  console.log(failed ? failed + " failed" : "all passed");
  process.exit(failed ? 1 : 0);
}
main().catch((e) => { console.error(e); try { server.close(); } catch (x) {} process.exit(1); });
