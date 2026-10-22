#!/usr/bin/env node
"use strict";
// VK-02 acceptance 2 and 3: tools/www/rvgpu-worker.js, unmodified, with rvgpu.wasm in a Node Worker on Dawn's Node
// bindings (worker-shim.js). The same page smoke tests as gpu/webgpu/build/smoke-browser.js: TEST clear
// (smoke-clear.c) or draw (T2's smoke-draw.c: depth, textures, instancing, a compute pass, sample pixels).
//   ./node_modules/.bin/node smoke.js [DIR] [clear|draw]
// DIR (default: build/rvgpu-page of this worktree) holds rvgpu-worker.js and rvgpu/rvgpu.{js,wasm}; fetch it with
// gpu/webgpu/build/smoke-page.sh (VM build) or copy it from another worktree.
// Env: DAWN_OPTS=backend=vulkan,adapter=... (dawn.node options, comma separated), SMOKE_TIMEOUT_S.
const { Worker } = require("worker_threads");
const path = require("path");
const fs = require("fs");

const root = path.resolve(__dirname, "../../../..");
const dir = path.resolve(process.argv[2] || path.join(root, "build/rvgpu-page"));
const test = process.argv[3] || "draw";
// rvgpu-worker.js is always the tree's own copy
fs.copyFileSync(path.join(root, "tools/www/rvgpu-worker.js"), path.join(dir, "rvgpu-worker.js"));

const t0 = Date.now();
const w = new Worker(path.join(__dirname, "worker-shim.js"), {
  workerData: { dir, dawn: (process.env.DAWN_OPTS || "").split(",").filter(Boolean) },
  execArgv: [], stdout: false, stderr: false,
});
const timer = setTimeout(() => { console.log("FAIL timeout"); process.exit(3); }, +(process.env.SMOKE_TIMEOUT_S || 120) * 1000);
w.on("error", (e) => { console.log("FAIL worker error:", e && e.stack || e); process.exit(1); });
w.on("exit", (c) => { if (c) { console.log("FAIL worker exit", c); process.exit(1); } });
w.on("message", (m) => {
  if (m.type === "shim-ready") {
    console.log("shim up", Date.now() - t0, "ms");
    w.postMessage({ type: "smoke", canvas: true, base: dir + "/", test, color: [0.25, 0.5, 0.75, 1] });
  } else if (m.type === "log") console.log("  log:", m.text);
  else if (m.type === "smoke") {
    clearTimeout(timer);
    console.log(JSON.stringify(m, null, 1));
    const px = m.readback;
    let ok = m.ok;
    if (test === "clear" && px) ok = ok && px.join() === "64,128,191,255";	// (0.25, 0.5, 0.75) * 255, rounded
    console.log((ok ? "PASS" : "FAIL") + " " + test + " in " + (Date.now() - t0) + " ms");
    process.exit(ok ? 0 : 1);
  }
});
