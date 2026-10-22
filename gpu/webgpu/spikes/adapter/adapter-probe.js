#!/usr/bin/env node
// What the test browser's WebGPU really offers on this Mac, and what its calls cost: adapter features and limits,
// WGSL language features, a device with everything requested, then four timings (encoding cost per draw,
// writeBuffer bandwidth, submit -> onSubmittedWorkDone and submit -> mapAsync round trips).
// Prints one JSON object per browser configuration.
//
//   tools/with-browser.sh WHO node gpu/webgpu/spikes/adapter/adapter-probe.js [EXTRA_CHROME_FLAG ...]
const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

if (process.env.RV_BROWSER != "1") {
  console.error("run this under tools/with-browser.sh (one test browser at a time)");
  process.exit(2);
}
const CHROME = process.env.CHROME || path.join(__dirname, "../../../../tools/chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
const extra = process.argv.slice(2);

const PROBE = `(async () => {
  const out = { secure: isSecureContext, ua: navigator.userAgent };
  if (!navigator.gpu) return { ...out, error: "no navigator.gpu" };
  out.wgslLanguageFeatures = [...navigator.gpu.wgslLanguageFeatures].sort();
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) return { ...out, error: "no adapter" };
  out.info = {}; for (const k in adapter.info) out.info[k] = adapter.info[k];
  out.features = [...adapter.features].sort();
  out.limits = {}; for (const k in adapter.limits) out.limits[k] = adapter.limits[k];
  const requiredLimits = {};
  for (const k in adapter.limits) requiredLimits[k] = adapter.limits[k];
  let device;
  try { device = await adapter.requestDevice({ requiredFeatures: out.features, requiredLimits }); }
  catch (e) { out.deviceError = String(e); device = await adapter.requestDevice({ requiredFeatures: out.features }); }
  out.api = {
    setImmediates: typeof GPURenderPassEncoder.prototype.setImmediates,
    mapSync: typeof GPUBuffer.prototype.mapSync,
    multiDrawIndirect: typeof GPURenderPassEncoder.prototype.multiDrawIndirect,
    createResourceTable: typeof device.createResourceTable,
  };
  // Timings
  const t = {};
  const fmt = "rgba8unorm";
  const tex = device.createTexture({ size: [256, 256], format: fmt, usage: GPUTextureUsage.RENDER_ATTACHMENT });
  const mod = device.createShaderModule({ code: "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { return vec4f(f32(i & 1u), f32(i >> 1u), 0.5, 1.0); } @group(0) @binding(0) var<uniform> c: vec4f; @fragment fn fs() -> @location(0) vec4f { return c; }" });
  const pipe = device.createRenderPipeline({ layout: "auto", vertex: { module: mod, entryPoint: "vs" }, fragment: { module: mod, entryPoint: "fs", targets: [{ format: fmt }] }, primitive: { topology: "triangle-list" } });
  const ubuf = device.createBuffer({ size: 256 * 1024, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const bgs = []; for (let i = 0; i < 64; i++) bgs.push(device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: ubuf, offset: i * 256, size: 16 } }] }));
  const encodeDraws = (n) => { const e = device.createCommandEncoder(); const p = e.beginRenderPass({ colorAttachments: [{ view: tex.createView(), loadOp: "clear", storeOp: "store" }] }); p.setPipeline(pipe); for (let i = 0; i < n; i++) { p.setBindGroup(0, bgs[i & 63]); p.draw(3); } p.end(); return e.finish(); };
  device.queue.submit([encodeDraws(1000)]); await device.queue.onSubmittedWorkDone();
  let t0 = performance.now(); const cb = encodeDraws(20000); t.encode_us_per_setBindGroup_plus_draw = (performance.now() - t0) * 1000 / 20000;
  t0 = performance.now(); device.queue.submit([cb]); await device.queue.onSubmittedWorkDone(); t.submit_and_run_20000_draws_ms = performance.now() - t0;
  const big = device.createBuffer({ size: 64 << 20, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
  const src = new Uint8Array(64 << 20);
  device.queue.writeBuffer(big, 0, src); await device.queue.onSubmittedWorkDone();
  t0 = performance.now(); for (let i = 0; i < 8; i++) device.queue.writeBuffer(big, 0, src); await device.queue.onSubmittedWorkDone();
  t.writeBuffer_MBps = 8 * 64 / ((performance.now() - t0) / 1000);
  const small = []; for (let i = 0; i < 3; i++) small.push(4096 << (i * 4));
  const lat = []; for (let i = 0; i < 50; i++) { const t1 = performance.now(); device.queue.submit([device.createCommandEncoder().finish()]); await device.queue.onSubmittedWorkDone(); lat.push(performance.now() - t1); }
  lat.sort((a, b) => a - b); t.workDone_rtt_ms_p50 = lat[25]; t.workDone_rtt_ms_p90 = lat[45];
  const rb = device.createBuffer({ size: 4096, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  const ml = []; for (let i = 0; i < 50; i++) { const t1 = performance.now(); const e = device.createCommandEncoder(); e.copyBufferToBuffer(big, 0, rb, 0, 4096); device.queue.submit([e.finish()]); await rb.mapAsync(GPUMapMode.READ); rb.getMappedRange(); rb.unmap(); ml.push(performance.now() - t1); }
  ml.sort((a, b) => a - b); t.copy_mapAsync_4KB_rtt_ms_p50 = ml[25]; t.copy_mapAsync_4KB_rtt_ms_p90 = ml[45];
  const rb2 = device.createBuffer({ size: 8 << 20, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  const ml2 = []; for (let i = 0; i < 10; i++) { const t1 = performance.now(); const e = device.createCommandEncoder(); e.copyBufferToBuffer(big, 0, rb2, 0, 8 << 20); device.queue.submit([e.finish()]); await rb2.mapAsync(GPUMapMode.READ); new Uint8Array(rb2.getMappedRange()).slice(0, 1); rb2.unmap(); ml2.push(performance.now() - t1); }
  ml2.sort((a, b) => a - b); t.copy_mapAsync_8MB_rtt_ms_p50 = ml2[5];
  const shaders = []; for (let i = 0; i < 20; i++) { const t1 = performance.now(); const m = device.createShaderModule({ code: "@group(0) @binding(0) var<storage, read_write> b: array<f32>; @compute @workgroup_size(64) fn main(@builtin(global_invocation_id) g: vec3u) { var x = b[g.x]; for (var k = 0; k < " + (8 + i) + "; k++) { x = sin(x) * 1.5 + cos(x * " + (i + 1) + ".0); } b[g.x] = x; }" }); await device.createComputePipelineAsync({ layout: "auto", compute: { module: m, entryPoint: "main" } }); shaders.push(performance.now() - t1); }
  shaders.sort((a, b) => a - b); t.small_compute_pipeline_compile_ms_p50 = shaders[10];
  out.timings = t;
  return out;
})()`;

async function run(flags) {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "rvwebgpu-"));
  const chrome = spawn(CHROME, ["--headless=new", "--user-data-dir=" + profile, "--remote-debugging-port=0", "--no-first-run",
    "--no-default-browser-check", ...flags, "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
  const kill = () => { try { chrome.kill(); } catch (e) { /* gone */ } };
  process.on("exit", kill);
  const address = await new Promise((resolve, reject) => {
    let err = "";
    chrome.stderr.on("data", (d) => { err += d; const m = err.match(/DevTools listening on (ws:\/\/\S+)/); if (m) resolve(m[1]); });
    chrome.on("exit", () => reject(new Error("Chrome exited: " + err)));
  });
  const ws = new WebSocket(address);
  const waiting = new Map(); let next = 1;
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && waiting.has(m.id)) { const w = waiting.get(m.id); waiting.delete(m.id); m.error ? w.reject(new Error(m.error.message)) : w.resolve(m.result); } };
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = () => reject(new Error("cannot connect")); });
  const send = (method, params = {}, sessionId) => { const id = next++; ws.send(JSON.stringify({ id, method, params, sessionId })); return new Promise((resolve, reject) => waiting.set(id, { resolve, reject })); };
  const { targetId } = await send("Target.createTarget", { url: process.env.PROBE_URL || "http://localhost:8000/" });
  const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
  await new Promise((r) => setTimeout(r, 1500));
  const r = await send("Runtime.evaluate", { expression: PROBE, awaitPromise: true, returnByValue: true }, sessionId);
  console.log(JSON.stringify({ flags, result: r.exceptionDetails ? { error: r.exceptionDetails.exception?.description || r.exceptionDetails.text } : r.result.value }));
  ws.close(); kill();
  await new Promise((r) => setTimeout(r, 500));
  try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) { /* still writing */ }
}

(async () => {
  await run(extra);
  if (!extra.length) await run(["--enable-unsafe-webgpu", "--enable-blink-features=WebGPUExperimentalFeatures,WebGPUMapSyncOnWorkers,WebGPUExperimentalResourceTable"]);
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(1); });
