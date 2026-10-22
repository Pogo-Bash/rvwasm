#!/usr/bin/env node
// The page smoke test in the test browser: serve DIR (rvgpu-smoke.html, rvgpu-worker.js, rvgpu/rvgpu.{js,wasm})
// from a server of its own (COOP/COEP; the shared dev server :8000 is not touched), open the page in headless
// Chrome for Testing, wait for the Worker's result, save a screenshot, print the result. Gives up after 3 min.
//
//   tools/with-browser.sh WHO node gpu/webgpu/build/smoke-browser.js DIR [SHOT.png] [TEST]
// TEST (default clear): the page's ?test= (clear: smoke-clear.c; draw: T2's smoke-draw.c)
// SMOKE_RELOAD=1: after the result, load the page again in the same profile (IndexedDB kept: the pipeline
// cache's second run) and report both results.
// SMOKE_PAGE names another page of DIR that sets window.rvgpuSmoke (rvgpu-vfront.html: vfront's capture
// replay), SMOKE_WAIT_S how long to wait for it (default 60).
// (gpu/webgpu/build/smoke-page.sh fetches the build from the VM into DIR and runs this.)
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
const dir = path.resolve(process.argv[2] || ".");
const shot = process.argv[3];
const test = process.argv[4] || "clear";
const CHROME = process.env.CHROME || "${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing";
const page = process.env.SMOKE_PAGE || "rvgpu-smoke.html";
const waitS = +(process.env.SMOKE_WAIT_S || 60);
let lastText = "", stage = "start";	/* for the give-up message */
setTimeout(() => {
  console.error("smoke-browser: gave up (at: " + stage + "); page log so far:\n" + lastText);
  process.exit(3);
}, (waitS + 120) * 1000).unref();

const types = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm", ".png": "image/png" };
// (anything else, captures included, goes as application/octet-stream)
const server = http.createServer((req, res) => {
  const p = path.join(dir, decodeURIComponent(new URL(req.url, "http://x").pathname));
  if (!p.startsWith(dir) || !fs.existsSync(p) || fs.statSync(p).isDirectory()) {
    res.writeHead(404);
    res.end();
    return;
  }
  res.writeHead(200, { "Content-Type": types[path.extname(p)] || "application/octet-stream",
                       "Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp",
                       "Cache-Control": "no-store" });
  fs.createReadStream(p).pipe(res);
});

(async () => {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const url = "http://127.0.0.1:" + server.address().port + "/" + page + "?test=" + test;
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "rvgpu-smoke-"));
  const chrome = spawn(CHROME, ["--headless=new", "--user-data-dir=" + profile, "--remote-debugging-port=0",
    "--no-first-run", "--no-default-browser-check", "--window-size=800,800", "about:blank"],
    { stdio: ["ignore", "ignore", "pipe"] });
  const kill = () => { try { chrome.kill(); } catch (e) { /* gone */ } };
  process.on("exit", kill);
  const address = await new Promise((resolve, reject) => {
    let err = "";
    chrome.stderr.on("data", (d) => { err += d; const m = err.match(/DevTools listening on (ws:\/\/\S+)/); if (m) resolve(m[1]); });
    chrome.on("exit", () => reject(new Error("Chrome exited: " + err)));
  });
  const ws = new WebSocket(address);
  const waiting = new Map();
  let next = 1;
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && waiting.has(m.id)) {
      const w = waiting.get(m.id);
      waiting.delete(m.id);
      m.error ? w.reject(new Error(m.error.message)) : w.resolve(m.result);
    }
  };
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = () => reject(new Error("cannot connect")); });
  const send = (method, params = {}, sessionId) => {
    const id = next++;
    ws.send(JSON.stringify({ id, method, params, sessionId }));
    return new Promise((resolve, reject) => waiting.set(id, { resolve, reject }));
  };
  const { targetId } = await send("Target.createTarget", { url });
  const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
  let result = null;
  for (let i = 0; i < waitS * 2 && !result; i++) {
    await new Promise((r) => setTimeout(r, 500));
    stage = "waiting for the result, " + (i / 2) + " s";
    const r = await send("Runtime.evaluate", { expression: "window.rvgpuSmoke ? JSON.stringify(window.rvgpuSmoke) : ''",
                                               returnByValue: true }, sessionId);
    const t = await send("Runtime.evaluate", { expression: "document.getElementById('out').textContent",
                                               returnByValue: true }, sessionId);
    lastText = (t.result && t.result.value) || lastText;
    if (r.result && r.result.value) result = JSON.parse(r.result.value);
  }
  const text = await send("Runtime.evaluate", { expression: "document.getElementById('out').textContent",
                                                returnByValue: true }, sessionId);
  if (shot) {
    const s = await send("Page.captureScreenshot", { format: "png" }, sessionId);
    fs.writeFileSync(shot, Buffer.from(s.data, "base64"));
  }
  console.log("page log:\n" + (text.result && text.result.value));
  console.log("result: " + JSON.stringify(result));
  if (process.env.SMOKE_RELOAD === "1" && result) {
    // the same page again in the same profile: IndexedDB (the pipeline cache) kept
    await new Promise((r) => setTimeout(r, 1500));	/* (the Worker's batched IndexedDB write) */
    stage = "reload";
    await send("Page.reload", {}, sessionId);
    let again = null;
    for (let i = 0; i < waitS * 2 && !again; i++) {
      await new Promise((r) => setTimeout(r, 500));
      const r = await send("Runtime.evaluate", { expression: "window.rvgpuSmoke ? JSON.stringify(window.rvgpuSmoke) : ''",
                                                 returnByValue: true }, sessionId);
      if (r.result && r.result.value) again = JSON.parse(r.result.value);
    }
    console.log("result after reload: " + JSON.stringify(again));
    if (!again || !again.ok) result.ok = false;
  }
  ws.close();
  kill();
  server.close();
  await new Promise((r) => setTimeout(r, 500));
  try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) { /* still writing */ }
  process.exit(result && result.ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(1); });
