"use strict";
// VK-06: the entry of the Node Worker that runs tools/www/rvgpu-worker.js (the page's real GPU executor: vfront on
// zink on wgvk, and vkr, on a Dawn device) under tools/node-machine.js. It is VK-02's browser-Worker stand-in
// (gpu/webgpu/tests/dawn-node/worker-shim.js: navigator.gpu from Dawn's Node bindings, importScripts, fetch of local
// files, no `process`) plus what the page has and the shim's smoke tests did not need:
//
//   video frames      (VK-14) the decoder's frames come from gpu/tools/vdec-stub.js as plain descriptors (Node has no
//                     VideoFrame): this side makes them objects with the parts of VideoFrame that rvgpu-worker.js uses
//                     (copyTo, close, display size, colour space), and the device's importExternalTexture (which dawn.node
//                     does not take frames for) throws, so the Worker's own fallback runs: copyExternalImageToTexture,
//                     here a writeTexture of the frame's RGB as a browser converts it. The external-texture path itself is
//                     the page's, not tested here.
//   OffscreenCanvas   a "webgpu" context that is a texture on the device; transferToImageBitmap() copies it into a
//                     buffer. rvgpu-worker.js draws a window's GPU frame into it and posts the bitmap to the page
//                     ("present"); there is no page here, so the bitmap is read back (async, the same queue, so
//                     in order) and posted as {width, height, format, rgba: Uint8Array} (RGBA, rows tight).
//
// workerData: { dir, dawn }   (dir: the directory with rvgpu-worker.js and rvgpu/rvgpu.{js,wasm}; dawn: dawn.node options)
// It is started by gpu/tools/rvgpu-nm.js, the hook. It is not a file to run by hand.
const { parentPort } = require("worker_threads");
const path = require("path");

// (the shim deletes the global `process`; the bits wanted afterwards are taken first)
const shim = path.resolve(__dirname, "../webgpu/tests/dawn-node/worker-shim.js");
// (VK-14: the shim's message listener passes the decoder's MessagePort on as it came; ours sits in front of it, so that the
// frames that arrive on it are made into VideoFrame stand-ins: see wrapPort below)
const { SynthFrame } = require("./synth-frame.js");
const wrapPort = (port) => ({
  postMessage: (...a) => port.postMessage(...a),
  set onmessage(fn) {
    port.onmessage = fn ? (e) => {
      const d = e.data;
      fn({ data: d && d.frame && d.frame.synthetic ? { ...d, frame: new SynthFrame(d.frame) } : d });
    } : null;
  },
});
const portOn = parentPort.on;
parentPort.on = function (ev, fn) {
  return ev === "message" ? portOn.call(this, ev, (m) => { if (m && m.type === "vdec-port" && m.port) m.port = wrapPort(m.port); fn(m); }) : portOn.call(this, ev, fn);
};
require(shim);
parentPort.on = portOn;

const align = (n, a) => Math.ceil(n / a) * a;

class OffscreenCanvas {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this._ctx = null;
  }

  getContext(kind) {
    if (kind !== "webgpu") return null;
    if (this._ctx) return this._ctx;
    const canvas = this;
    let cfg = null, tex = null;
    this._ctx = {
      canvas,
      configure(o) {
        cfg = o;
        tex = null;
      },
      unconfigure() {
        cfg = null;
        tex = null;
      },
      getCurrentTexture() {
        if (!cfg) throw new Error("OffscreenCanvas shim: getCurrentTexture before configure");
        if (!tex || tex.width !== canvas.width || tex.height !== canvas.height) {
          tex = cfg.device.createTexture({
            size: [canvas.width, canvas.height], format: cfg.format,
            usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING,
            viewFormats: cfg.viewFormats || [],
          });
        }
        return tex;
      },
      _take() {
        const t = tex;
        tex = null;	// (a canvas that has handed its frame over starts a new one)
        return { tex: t, device: cfg && cfg.device, format: cfg && cfg.format };
      },
    };
    return this._ctx;
  }

  transferToImageBitmap() {
    const { tex, device, format } = this._ctx._take();
    if (!tex) throw new Error("OffscreenCanvas shim: transferToImageBitmap without a frame drawn");
    const w = tex.width, h = tex.height, bpr = align(w * 4, 256);
    const buf = device.createBuffer({ size: bpr * h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: bpr }, [w, h]);
    device.queue.submit([enc.finish()]);
    return {
      width: w, height: h, format,
      close() {},
      // (the rows of the readback, tight, as RGBA: the bytes of a bgra format are swapped here)
      async read() {
        await buf.mapAsync(GPUMapMode.READ);
        const src = new Uint8Array(buf.getMappedRange());
        const out = new Uint8Array(w * h * 4);
        const bgra = /^bgra/.test(format);
        for (let y = 0; y < h; y++) {
          const row = src.subarray(y * bpr, y * bpr + w * 4);
          if (!bgra) {
            out.set(row, y * w * 4);
          } else {
            for (let x = 0; x < w * 4; x += 4) {
              const d = y * w * 4 + x;
              out[d] = row[x + 2];
              out[d + 1] = row[x + 1];
              out[d + 2] = row[x];
              out[d + 3] = row[x + 3];
            }
          }
        }
        buf.unmap();
        buf.destroy();
        tex.destroy();
        return out;
      },
    };
  }
}
globalThis.OffscreenCanvas = OffscreenCanvas;

// "present" carries an ImageBitmap in the page; here its readback, once the GPU has it.
const post = globalThis.postMessage;
let lastWaits = -1;
globalThis.postMessage = (m, transfer) => {
  // (the wasm heap's debug words: [4] = wgvk_wait calls so far, each a JSPI suspension; sampled for the summary)
  if (m && m.type === "debug" && m.heap && m.words) {
    const heap = m.heap, at = m.words >> 2;
    setInterval(() => {
      if (!heap.byteLength) return;	// (a grown heap detaches the old buffer: the last count stays)
      const w = new Uint32Array(heap, 0, (at + 8) | 0);
      if (w[at + 4] !== lastWaits) post({ type: "nm-waits", waits: (lastWaits = w[at + 4]) });
    }, 100).unref();
  }
  if (m && m.type === "present" && m.bitmap && typeof m.bitmap.read === "function") {
    const b = m.bitmap;
    b.read().then((rgba) => {
      post({ ...m, bitmap: { width: b.width, height: b.height, format: b.format, rgba } }, [rgba.buffer]);
    }, (e) => post({ type: "log", text: "OffscreenCanvas shim: readback failed: " + (e && e.stack || e) }));
    return;
  }
  post(m, (transfer || []).filter((t) => t instanceof ArrayBuffer || t instanceof MessagePort));
};

// The device rvgpu-worker.js makes, kept for "nm-destroy-device" (RVGPU_NM_DEVICE_LOSS in the hook, VK-13's loss path:
// device.destroy() is what a lost device looks like to the executor).
let theDevice = null;
{
  const gpu = navigator.gpu;
  const requestAdapter = gpu.requestAdapter.bind(gpu);
  gpu.requestAdapter = async (...a) => {
    const adapter = await requestAdapter(...a);
    if (adapter) {
      const requestDevice = adapter.requestDevice.bind(adapter);
      adapter.requestDevice = async (...b) => {
        const d = (theDevice = await requestDevice(...b));
        d.importExternalTexture = () => { throw new TypeError("dawn.node takes no VideoFrame (the stand-in decoder's frames are descriptors)"); };
        d.queue.copyExternalImageToTexture = (src, dst, size) => {
          const f = src && src.source;
          if (!f || typeof f.rgba !== "function") throw new TypeError("copyExternalImageToTexture: not a frame of the stand-in decoder");
          d.queue.writeTexture({ texture: dst.texture }, f.rgba(), { bytesPerRow: f.displayWidth * 4 }, { width: f.displayWidth, height: f.displayHeight });
        };
        return d;
      };
    }
    return adapter;
  };
}
parentPort.on("message", (m) => {
  if (m && m.type === "nm-destroy-device" && theDevice) theDevice.destroy();
  // (the executor's counters now, for the summary: P is rvgpu-worker.js's, a script-level const in this context)
  if (m && m.type === "nm-stats") {
    try {
      post({ type: "stats", stats: JSON.parse(JSON.stringify(require("vm").runInThisContext("P.stats"))) });
    } catch (e) { /* (not started) */ }
    post({ type: "nm-stats-done" });
  }
});
