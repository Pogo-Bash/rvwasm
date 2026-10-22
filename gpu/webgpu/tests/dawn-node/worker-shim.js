"use strict";
// VK-02: a Node worker_threads Worker that looks enough like a browser dedicated Worker for tools/www/rvgpu-worker.js
// (unmodified) and rvgpu.js (Emscripten, -sENVIRONMENT=web,worker, emdawnwebgpu, JSPI) to run, with
// navigator.gpu from Dawn's Node bindings (the `webgpu` npm package: dawn.node).
//
// workerData: { dir, dawn: [dawn.node options] }   dir: the directory holding rvgpu-worker.js and rvgpu/ (a path).
// What the page worker expects and this provides:
//   self, self.postMessage, self.onmessage (parentPort messages), importScripts (synchronous, from dir),
//   navigator.gpu + the GPU* constants, WorkerGlobalScope (rvgpu.js picks its web-worker environment from it),
//   self.location, fetch of local paths (wasm streaming compile), and no `process` (rvgpu.js refuses to run
//   when it sees Node).
// The canvas of a "smoke" message is a stand-in (m.canvas === true): getContext("webgpu") makes a texture on the
// device, so the Worker's copyTextureToTexture / readback code runs; the pixel it reads is the result.
const { parentPort, workerData, threadId } = require("worker_threads");
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const proc = process;
const { create, globals } = require("webgpu");

const dir = path.resolve(workerData.dir);
const toPath = (u) => {
  u = String(u);
  if (u.startsWith("file://")) return decodeURIComponent(new URL(u).pathname);
  return path.isAbsolute(u) ? u : path.join(dir, u);
};

Object.assign(globalThis, globals);
globalThis.self = globalThis;
globalThis.WorkerGlobalScope = class WorkerGlobalScope {};
Object.setPrototypeOf(globalThis, WorkerGlobalScope.prototype);	// (self instanceof WorkerGlobalScope)
const gpu = create(workerData.dawn || []);
Object.defineProperty(globalThis, "navigator", { value: { gpu, hardwareConcurrency: 4, userAgent: "dawn.node" },
                                                 configurable: true, writable: true });
globalThis.location = { href: "file://" + dir + "/rvgpu-worker.js" };
globalThis.postMessage = (m, transfer) => parentPort.postMessage(m, transfer || []);
globalThis.importScripts = (...urls) => {
  for (const u of urls) {
    const p = toPath(u);
    vm.runInThisContext(fs.readFileSync(p, "utf8"), { filename: p });
  }
};
const realFetch = globalThis.fetch;
globalThis.fetch = async (u, init) => {
  const s = String(u && u.url || u);
  if (/^https?:/.test(s)) return realFetch(u, init);
  const p = toPath(s);
  if (!fs.existsSync(p)) return new Response(null, { status: 404 });
  const type = p.endsWith(".wasm") ? "application/wasm" : "application/octet-stream";
  return new Response(fs.readFileSync(p), { status: 200, headers: { "Content-Type": type } });
};
// (rvgpu.js: ENVIRONMENT_IS_NODE and its "not compiled for Node" check both look at `process`)
delete globalThis.process;

// A stand-in for the page's OffscreenCanvas (smoke only).
function fakeCanvas() {
  const c = { width: 300, height: 150, getContext(kind) {
    if (kind !== "webgpu") return null;
    let cfg = null, tex = null;
    return {
      configure(o) { cfg = o; },
      getCurrentTexture() {
        if (!tex || tex.width !== c.width || tex.height !== c.height)
          tex = cfg.device.createTexture({ size: [c.width, c.height], format: cfg.format,
                                           usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
        return tex;
      },
    };
  } };
  return c;
}

vm.runInThisContext(fs.readFileSync(path.join(dir, "rvgpu-worker.js"), "utf8"), { filename: path.join(dir, "rvgpu-worker.js") });
const handler = globalThis.onmessage;
parentPort.on("message", (data) => {
  if (data && data.type === "smoke" && data.canvas === true) data = { ...data, canvas: fakeCanvas() };
  handler({ data });
});
parentPort.postMessage({ type: "shim-ready", threadId });
