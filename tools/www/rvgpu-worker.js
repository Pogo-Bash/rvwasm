// rvgpu-worker.js: the page Worker host of the GPU on WebGPU (gpu/webgpu/INTERFACES.md, section 8).
//
// It loads rvgpu.js/rvgpu.wasm (wgvk, and later vfront and vkr: gpu/webgpu/build/wasm.sh), owns the one
// GPUDevice (requested with every feature and the highest limits the adapter has), hands it to wgvk, and
// attaches to the machine's memory and gpuserver's ring. It speaks gpu-exec.js's Worker messages, so
// gpu-exec.js can start it in place of the vrend Worker:
//   page -> worker: {type:"start", memory, ring, caps:{at,size}, base}   (base: the URL directory of rvgpu/)
//                   {type:"grab", ctx, res, present}, {type:"shown", serial}, {type:"table", ...}
//                   {type:"smoke", canvas, base, color}   (tools/www/rvgpu-smoke.html: no machine)
//                   {type:"vfront-replay", base, url}   (tools/www/rvgpu-vfront.html: a virgl capture replayed into
//                   vfront in the module, zink on wgvk on this device; no machine)
//                   {type:"imported", res, frame}   (the page took the bitmap of that present: the next may come)
//                   {type:"vdec-port", port}   (VK-14: the video decoder's Worker, tools/www/vdec-worker.js: it posts
//                   {type:"vdec-frame", frame, res:[luma, chroma], w, h, ready, seq, exact} on that port, a decoded
//                   VideoFrame to draw into a guest surface's two GPU resources; see "video frames" below)
//   worker -> page: {type:"ready", renderer, exec:"webgpu", capsLen, errors, fallback, build}   (build: the rvgpu-stamp), {type:"log", text},
//                   {type:"present", ctx, res, frame, serial, bitmap, w, h, grabbed, bgra, blitMs, stats},
//                   {type:"stats", stats} (stats.vdec* count the video frames), {type:"smoke", ok, msg, device, readback}
//                   {type:"vfront-replay", ok, code, out, ms, errors}
// fallback: true when this browser gives no WebGPU device (nothing was attached: the page starts vrend's
// Worker on the same ring instead; a "vdec-port" this Worker already holds goes back to the page in that message, as
// `vdecPort`, for the vrend Worker).
// Presenting (T7, INTERFACES.md 9; gpu/PROTOCOL.md, stage B): a window whose buffer is a GPU resource
// (wlserver's slot format 2) is drawn from that resource's texture, with no readback: this Worker watches the
// window server's surface table, and once the ring's `done` has passed a slot's gpu_serial it asks rvgpu for
// the resource's texture (rvgpu_present_grab: vfront's zink image, or a venus WSI image), draws it into its
// OffscreenCanvas (a webgpu context: GPU to GPU, the right way up, any colour format), and posts the canvas
// as an ImageBitmap (transferToImageBitmap), as gpu-exec.js's vrend Worker does. One frame in flight per
// resource: the next is drawn once the page has said "imported" (or after 250 ms without word).
// The ring loop never blocks this thread (WebGPU's callbacks run from its event loop): Atomics.waitAsync on
// the doorbell, then rvgpu_ring_drain(), which returns a Promise (JSPI: it may wait for the GPU inside).
"use strict";

let M = null, device = null, adapterInfo = "";
// The build stamp of rvgpu.wasm (VK-04): the custom section "rvgpu-stamp" (tools/rvgpu-stamp.js writes and reads
// it): {branch, commit, mesa, dawn, patch, dirty}. null when the binary has none (an unstamped build).
let stamp = null;
const errors = [];
const log = (text) => self.postMessage({ type: "log", text });

async function makeDevice() {
  if (!navigator.gpu) throw new Error("no navigator.gpu in this Worker");
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) throw new Error("no WebGPU adapter");
  const requiredLimits = {};
  for (const k in adapter.limits) requiredLimits[k] = adapter.limits[k];
  const requiredFeatures = [...adapter.features];
  const d = await adapter.requestDevice({ requiredFeatures, requiredLimits });
  d.addEventListener("uncapturederror", (ev) => {
    const t = "WebGPU error: " + ev.error.message;
    if (errors.length < 50) errors.push(t);
    log(t);
  });
  d.lost.then((info) => log("WebGPU device lost: " + info.reason + " " + info.message));
  const i = adapter.info || {};
  adapterInfo = [i.vendor, i.architecture, i.device, i.description].filter(Boolean).join(" / ");
  return d;
}

// (T2) The persistent pipeline cache: wgvk hands a record per new render pipeline (Module.rvgpuCacheStore);
// they go to IndexedDB in batches, and at the next start back into wgvk before any device is made, so the
// pipelines are created (asynchronously) before the first draw needs them. Records of another wgvk version are
// skipped by wgvk; when none of the stored ones is usable the store is emptied.
const CACHE_DB = "rvgpu-pipeline-cache", CACHE_MAX = 64 << 20;
let cacheDb = null, cachePending = [], cacheTimer = 0, cacheStats = { loaded: 0, stored: 0, bytes: 0 };
function idb(req) { return new Promise((res, rej) => { req.onsuccess = () => res(req.result); req.onerror = () => rej(req.error); }); }
async function cacheOpen() {
  if (!self.indexedDB) return null;
  const r = indexedDB.open(CACHE_DB, 1);
  r.onupgradeneeded = () => r.result.createObjectStore("records", { autoIncrement: true });
  return idb(r);
}
async function cacheLoad() {
  try {
    cacheDb = await cacheOpen();
    if (!cacheDb) return null;
    const all = await idb(cacheDb.transaction("records").objectStore("records").getAll());
    let size = 0;
    const take = [];
    for (const rec of all) {
      if (size + rec.byteLength > CACHE_MAX) break;
      take.push(rec);
      size += rec.byteLength;
    }
    const blob = new Uint8Array(size);
    let at = 0;
    for (const rec of take) { blob.set(rec, at); at += rec.byteLength; }
    cacheStats.loaded = take.length;
    cacheStats.bytes = size;
    return blob;
  } catch (e) {
    log("pipeline cache: " + e);
    return null;
  }
}
function cacheFlush() {
  cacheTimer = 0;
  if (!cacheDb || !cachePending.length) return;
  const recs = cachePending;
  cachePending = [];
  try {
    const st = cacheDb.transaction("records", "readwrite").objectStore("records");
    for (const r of recs) st.add(r);
  } catch (e) {
    log("pipeline cache: " + e);
  }
}
function cacheStore(bytes) {
  cachePending.push(bytes);
  cacheStats.stored++;
  if (!cacheTimer) cacheTimer = setTimeout(cacheFlush, 500);
}

function readStamp(mod) {
  try {
    const c = WebAssembly.Module.customSections(mod, "rvgpu-stamp");
    return c.length ? JSON.parse(new TextDecoder().decode(c[c.length - 1])) : null;
  } catch (e) {
    return null;
  }
}
function stampText(st) {
  if (!st) return "UNSTAMPED (no rvgpu-stamp section)";
  return st.branch + " " + String(st.commit).slice(0, 12) + (st.dirty ? " DIRTY" : "") + " mesa " + String(st.mesa).slice(0, 10) +
         " dawn " + String(st.dawn).slice(0, 10);
}
async function compileWasm(url) {
  if (WebAssembly.compileStreaming) {
    try { return await WebAssembly.compileStreaming(fetch(url)); } catch (e) { /* (not a streaming-capable Response: below) */ }
  }
  return WebAssembly.compile(await (await fetch(url)).arrayBuffer());
}

async function boot(base, memory) {
  if (M) return M;
  if (!device)
    device = await makeDevice();
  importScripts(base + "rvgpu/rvgpu.js");
  let said = 0;
  const say = (t) => { if (said++ < 500) log("rvgpu: " + t); };
  M = await self.RvgpuModule({
    locateFile: (f) => base + "rvgpu/" + f,
    // (the module is compiled here, not by emscripten, only to read the stamp out of it)
    instantiateWasm: (imports, success) => {
      compileWasm(base + "rvgpu/rvgpu.wasm").then((mod) => {
        stamp = readStamp(mod);
        log("rvgpu: build " + stampText(stamp));
        return WebAssembly.instantiate(mod, imports).then((inst) => success(inst, mod));
      }).catch((e) => {
        log("rvgpu: wasm instantiate failed: " + String(e && e.stack || e));
        throw e;
      });
      return {};
    },
    print: say, printErr: say,
    rvgpuDevice: device,
    rvgpuMemory: memory || new SharedArrayBuffer(65536),
  });
  P.stats.build = stamp;   // (rvGpu.stats: the stamp rides with the executor's stats)
  if (M._rvgpu_init() !== 0) throw new Error("rvgpu_init failed");
  if (M._rvgpu_pipeline_cache) {
    const blob = await cacheLoad();
    M.rvgpuCacheStore = cacheStore;
    let p = 0;
    if (blob && blob.byteLength) {
      p = M._malloc(blob.byteLength);
      M.HEAPU8.set(blob, p);
    }
    M._rvgpu_pipeline_cache(p, blob ? blob.byteLength : 0);
    if (p) M._free(p);
  }
  return M;
}

// The ring loop (gpu/PROTOCOL.md: doorbell at header word 7).
// After each drain wgvk retires what WebGPU finished (rvgpu_poll: downloads into the guest's pages, syncs,
// completion words); while submissions are in flight the loop also wakes when the GPU is done, not only on the
// doorbell.
async function run(memory, ring) {
  const bell = new Int32Array(memory.buffer, ring + 28, 1);
  P.bell = bell;
  // A drain that waits long (for the GPU, inside wgvk: JSPI keeps this event loop running) says so, with what
  // the executor was doing; one that never lets this timer run is busy in the module instead.
  let drainAt = 0, said = 0;
  setInterval(() => {
    const t = performance.now();
    if (drainAt && t - drainAt > 2000 && t - said > 5000) {
      said = t;
      const where = M._rvgpu_debug_where ? M.UTF8ToString(M._rvgpu_debug_where()) : "";
      log("rvgpu: a ring drain has waited " + Math.round(t - drainAt) + " ms" + (where ? " (" + where + ")" : ""));
    }
  }, 1000);
  for (;;) {
    const seen = Atomics.load(bell, 0);
    try {
      drainAt = performance.now();
      P.draining = true;
      await M._rvgpu_ring_drain();
      P.draining = false;
      drainAt = 0;
    } catch (e) {
      // (a trap in the module: say what it was; the ring stops here, gpuserver sees the executor go)
      log("rvgpu: the ring drain failed: " + String(e && e.stack || e));
      throw e;
    }
    // (video frames waiting for the ring to be up to date: drawn now, between two drains)
    if (V.queue.length)
      vdecPump();
    const inflight = M._rvgpu_poll ? M._rvgpu_poll() : 0;
    // (a front end with work of its own in flight, vkr's rings, asks for the next drain soon: 1 ms)
    const w = Atomics.waitAsync(bell, 0, seen, M._rvgpu_poll_ms ? M._rvgpu_poll_ms() : 500);
    if (!w.async) continue;
    if (inflight) {
      // (the GPU's done, then a turn of the event loop for the mapAsync callbacks)
      const gpu = device.queue.onSubmittedWorkDone().then(() => new Promise((r) => setTimeout(r, 0)));
      await Promise.race([w.value, gpu]);
    } else {
      await w.value;
    }
  }
}

async function smoke(m) {
  const t0 = performance.now();
  await boot(m.base, null);
  const c = m.color || [0.25, 0.5, 0.75, 1];
  const r = m.test === "draw" ? await M._rvgpu_smoke_draw() : await M._rvgpu_smoke_clear(c[0], c[1], c[2], c[3]);
  const msg = M.UTF8ToString(M._rvgpu_smoke_msg());
  const dev = M.UTF8ToString(M._rvgpu_smoke_device());
  cacheFlush();
  const out = { type: "smoke", ok: r === 0, code: r, msg, device: dev, adapter: adapterInfo, errors,
                ms: Math.round(performance.now() - t0), cache: cacheStats,
                wgvk: M._rvgpu_smoke_stats ? M.UTF8ToString(M._rvgpu_smoke_stats()) : "" };
  const ptr = M._rvgpu_smoke_texture();
  if (ptr) {
    // wgvk's image as WebGPU sees it: onto the page's canvas, and one pixel read back on this side too.
    const tex = M.rvgpuJsObject(ptr);
    out.texture = { width: tex.width, height: tex.height, format: tex.format };
    const ctx = m.canvas.getContext("webgpu");
    ctx.configure({ device, format: tex.format, usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
                    alphaMode: "opaque" });
    m.canvas.width = tex.width;
    m.canvas.height = tex.height;
    const buf = device.createBuffer({ size: 256 * tex.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToTexture({ texture: tex }, { texture: ctx.getCurrentTexture() }, [tex.width, tex.height]);
    enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: 256 }, [tex.width, tex.height]);
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const px = new Uint8Array(buf.getMappedRange());
    const mid = (tex.height >> 1) * 256 + (tex.width >> 1) * 4;
    out.readback = [px[mid], px[mid + 1], px[mid + 2], px[mid + 3]];
    buf.unmap();
  }
  self.postMessage(out);
}

// vfront's capture replay (gpu/webgpu/mesa/src/webgpu/vfront/vf_rvgpu.c, vfront_replay_page): what
// vfront-replay prints natively, here with zink on wgvk on WebGPU.
async function vfrontReplay(m) {
  const t0 = performance.now();
  await boot(m.base, null);
  const r = await fetch(m.url);
  if (!r.ok) throw new Error("capture " + m.url + ": " + r.status);
  M.FS.writeFile("/cap.vfcap", new Uint8Array(await r.arrayBuffer()));
  const code = await M._vfront_replay_page();
  self.postMessage({ type: "vfront-replay", ok: code === 0, code, adapter: adapterInfo, errors,
                     ms: Math.round(performance.now() - t0) });
}

// ---- presenting (T7)
// A byte address of the machine's memory as the index of its 32-bit word. That memory can be larger than 4 GiB (?mem=8192,
// the default on a big computer), so an address is an exact Number of up to 2^34 and more: never `>> 2` on one (from 2 GiB
// it goes negative, from 4 GiB it wraps to a LOW address, the kernel's). (Pointers into the module's own heap, rvgpu.wasm's,
// are 32-bit and below 2 GiB: its maximum memory: HEAPU32[p >> 2] is right for those.)
const ix32 = (address) => Math.floor(address / 4);
const S_SEQ = 0, S_ID = 4, S_PRESENT = 56, S_BUFFER_LO = 60, S_BUFFER_HI = 64, S_FORMAT = 72, S_GPU_SERIAL = 244;
const H_DONE = 68, FORMAT_GPU = 2, Y_0_TOP = 1, PRESENT_BUSY = 0x80000000, BUSY = "busy";
const P = {
  memory: null, ring: 0, table: null, timer: 0,
  sent: new Map(),	// slot -> the present last drawn for it
  inflight: new Map(),	// res -> {frame, t}: posted, the page has not said "imported" yet
  wanted: new Map(),	// res -> {ctx, frame}: a newer frame that came while one was in flight
  canvas: null, ctx: null, format: "", configured: "", pipelines: new Map(), layout: null,
  draining: false, bell: null,
  stats: { presents: 0, blitMs: 0, lastBlitMs: 0, maxBlitMs: 0, held: 0, missing: 0, unshowable: 0, busy: 0,
           rounds: 0, roundMs: 0, lastSize: "",
           // video frames (VK-14), read on the page as rvGpu.stats.exec: frames received, drawn into the guest's
           // surfaces (by shader from an external texture / a copied RGBA texture, or the planes' bytes written),
           // given back undrawn, failed; milliseconds in drawing (the exact path: including the readback)
           vdecFrames: 0, vdecDrawn: 0, vdecExternal: 0, vdecRgba: 0, vdecExact: 0, vdecSkipped: 0, vdecErrors: 0,
           vdecMs: 0, vdecLastMs: 0, vdecLast: "", vdecPath: "", vdecError: "" },
};

function watchTable() {
  const t = P.table;
  if (!t || !P.memory || !M)
    return;
  const w32 = new Int32Array(P.memory.buffer);
  if (t.address + t.header + t.slots * t.slotSize > w32.byteLength)
    return;
  const done = Atomics.load(w32, ix32(P.ring + H_DONE));
  for (let i = 0; i < t.slots; i++) {
    const at = t.address + t.header + i * t.slotSize;
    const seq = Atomics.load(w32, ix32(at));
    if ((seq & 1) || !w32[ix32(at + S_ID)] || w32[ix32(at + S_FORMAT)] !== FORMAT_GPU)
      continue;
    const frame = w32[ix32(at + S_PRESENT)] >>> 0, res = w32[ix32(at + S_BUFFER_LO)] >>> 0;
    const pid = w32[ix32(at + S_BUFFER_HI)] >>> 0, serial = w32[ix32(at + S_GPU_SERIAL)];
    if (Atomics.load(w32, ix32(at)) !== seq || (t.gpuPid && pid !== t.gpuPid) || P.sent.get(i) === frame
        || ((done - serial) | 0) < 0)
      continue;
    P.sent.set(i, frame);
    if (present(0, res, frame) === BUSY)
      P.sent.delete(i);	// (the ring is mid-record: again on the next look)
  }
}

// What the canvas draws a texture with: a triangle over the canvas, each pixel loaded from the texture (the
// same size: no filtering), the rows turned round unless the texture is top row first.
const BLIT_WGSL = `
@group(0) @binding(0) var src: texture_2d<f32>;
override flip: bool = true;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  return vec4f(f32((i << 1u) & 2u) * 2.0 - 1.0, f32(i & 2u) * 2.0 - 1.0, 0.0, 1.0);
}
@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  var c = vec2i(pos.xy);
  if (flip) {
    c.y = i32(textureDimensions(src).y) - 1 - c.y;
  }
  return vec4f(textureLoad(src, c, 0).rgb, 1.0);
}`;

function blitPipeline(format, flip) {
  const key = format + (flip ? "/flip" : "");
  let p = P.pipelines.get(key);
  if (!p) {
    if (!P.layout)
      P.layout = device.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.FRAGMENT,
                                                           texture: { sampleType: "unfilterable-float" } }] });
    const module = device.createShaderModule({ code: BLIT_WGSL });
    p = device.createRenderPipeline({
      layout: device.createPipelineLayout({ bindGroupLayouts: [P.layout] }),
      vertex: { module, entryPoint: "vs" },
      fragment: { module, entryPoint: "fs", targets: [{ format }], constants: { flip: flip ? 1 : 0 } },
      primitive: { topology: "triangle-list" },
    });
    P.pipelines.set(key, p);
  }
  return p;
}

// Formats a window can be: colour formats a shader reads as floats (not depth, stencil or integers).
const showable = (f) => !/^(depth|stencil)|int$/.test(f);

function present(ctx, res, frame) {
  const f = P.inflight.get(res);
  if (f && performance.now() - f.t < 250) {
    P.wanted.set(res, { ctx, frame });
    P.stats.held++;
    return;
  }
  const t0 = performance.now();
  const p = M._rvgpu_present_grab(res);
  if (!p) {
    P.stats.missing++;
    return;
  }
  const words = M.HEAPU32;
  const w = words[(p >> 2) + 1], h = words[(p >> 2) + 2], flags = words[(p >> 2) + 3];
  if (!words[p >> 2] && flags === PRESENT_BUSY) {
    P.stats.busy++;
    return BUSY;
  }
  const tex = M.rvgpuJsObject(words[p >> 2]);
  if (!tex || !w || !h || tex.dimension !== "2d" || tex.sampleCount > 1 || !showable(tex.format)) {
    P.stats.unshowable++;
    return;
  }
  if (!P.canvas) {
    P.canvas = new OffscreenCanvas(w, h);
    P.ctx = P.canvas.getContext("webgpu");
    P.format = navigator.gpu.getPreferredCanvasFormat();
  }
  if (P.canvas.width !== w || P.canvas.height !== h) {
    P.canvas.width = w;
    P.canvas.height = h;
  }
  // (an sRGB texture reads back linear: written through an sRGB view of the canvas, it comes out as it was)
  const srgb = tex.format.endsWith("-srgb");
  const target = srgb ? P.format + "-srgb" : P.format;
  if (P.configured !== target) {
    P.ctx.configure({ device, format: P.format, usage: GPUTextureUsage.RENDER_ATTACHMENT, alphaMode: "opaque",
                      viewFormats: srgb ? [target] : [] });
    P.configured = target;
  }
  const view = P.ctx.getCurrentTexture().createView({ format: target });
  const enc = device.createCommandEncoder();
  const pass = enc.beginRenderPass({ colorAttachments: [{ view, loadOp: "clear", storeOp: "store",
                                                         clearValue: [0, 0, 0, 1] }] });
  pass.setPipeline(blitPipeline(target, !(flags & Y_0_TOP)));
  pass.setBindGroup(0, device.createBindGroup({ layout: P.layout, entries: [
    { binding: 0, resource: tex.createView({ dimension: "2d", baseMipLevel: 0, mipLevelCount: 1,
                                             baseArrayLayer: 0, arrayLayerCount: 1 }) }] }));
  pass.draw(3);
  pass.end();
  device.queue.submit([enc.finish()]);
  const bitmap = P.canvas.transferToImageBitmap();
  const dt = performance.now() - t0;
  const st = P.stats;
  st.presents++;
  st.blitMs += dt;
  st.lastBlitMs = dt;
  st.maxBlitMs = Math.max(st.maxBlitMs, dt);
  st.lastSize = w + "x" + h + " " + tex.format + (flags & Y_0_TOP ? "" : " flipped");
  P.inflight.set(res, { frame, t: performance.now() });
  self.postMessage({ type: "present", ctx, res, frame, serial: 0, bitmap, w, h, grabbed: true, bgra: false,
                     blitMs: dt, stats: st }, [bitmap]);
}


// ---- video frames (VK-14; docs/video-decode.md)
// The page's video decoder (tools/www/vdec-worker.js, WebCodecs) posts each decoded VideoFrame of a guest player's
// picture here over the MessagePort of "vdec-port", to be drawn into the picture's surface: two GPU resources the
// guest's driver made, luma (R8, w x h) and chroma (R8G8, w/2 x h/2), which the player's GL then samples through an
// EGL dmabuf import (zero copy). WebGPU gives no access to a frame's planes, so (as gpu-exec.js does for vrend with
// WebGL) the frame is read as RGB and turned back into Y and CbCr with the inverse of its colour-space matrix:
//   - the frame as a GPUExternalTexture (device.importExternalTexture: the decoder's own image, no copy), two render
//     passes (luma into the R8 texture, the mean of each 2x2 block into the R8G8 one) with a shader; if the browser
//     will not import it, copyExternalImageToTexture into an RGBA8 texture and the same passes from that;
//   - m.exact (?vdec=exact), or a frame whose visible rect is not its display size, or when the shader path fails:
//     VideoFrame.copyTo of the planes and queue.writeTexture (the bytes exactly; a readback and an upload).
// The textures are those of rvgpu_present_grab(res), as a window's resource is found: vfront's zink image of the
// virgl resource. Frames are drawn between two ring drains, so everything the guest sent before the frame (its draws
// from the surface's previous picture) has been recorded first, and everything it sends after the "ready" word runs
// after this (one queue, in order). The ready word is written whatever happened: the guest does not wait for a frame
// that failed.
const V = { port: null, memory: null, queue: [], busy: false, ubo: null, layouts: new Map(), pipelines: new Map(),
            noExternal: false, noShader: false, rgba: null, said: 0 };
const vlog = (t) => { if (V.said++ < 20) log("vdec: " + t); };

// The inverse of the browser's YUV to RGB for a frame's colour space (the matrix's Kr and Kb, Rec. ITU-T H.273): Y'CbCr
// from R'G'B' as a column-major mat3 and an offset, in 0..1 units (limited range: 16..235, 16..240). As gpu-exec.js's.
function vdecMatrix(cs) {
  const K = { bt709: [0.2126, 0.0722], bt470bg: [0.299, 0.114], smpte170m: [0.299, 0.114], "bt2020-ncl": [0.2627, 0.0593] };
  const full = !cs || cs.fullRange !== false;
  if (cs && cs.matrix === "rgb")
    return { m: new Float32Array([0, 0, 1, 1, 0, 0, 0, 1, 0]), off: new Float32Array([0, 0, 0]) };	// (G is Y, B is Cb, R is Cr)
  const [kr, kb] = K[cs && cs.matrix] || K.bt709, kg = 1 - kr - kb;
  const rows = [[kr, kg, kb], [-kr / (2 * (1 - kb)), -kg / (2 * (1 - kb)), 0.5], [0.5, -kg / (2 * (1 - kr)), -kb / (2 * (1 - kr))]];
  const sy = full ? 1 : 219 / 255, sc = full ? 1 : 224 / 255;
  const scale = [sy, sc, sc];
  const m = new Float32Array(9);
  for (let r = 0; r < 3; r++)
    for (let c = 0; c < 3; c++)
      m[c * 3 + r] = rows[r][c] * scale[r];
  return { m, off: new Float32Array([full ? 0 : 16 / 255, 128 / 255, 128 / 255]) };
}

// One triangle over the target; the fragment shaders read the frame at their own pixel (row 0 is the top, in the frame
// and in the surface's textures alike) and write Y, or the mean of the 2x2 block's Cb and Cr.
function vdecWgsl(external) {
  return `
struct U { c0: vec4f, c1: vec4f, c2: vec4f, off: vec4f, size: vec4f }
@group(0) @binding(0) var src: ${external ? "texture_external" : "texture_2d<f32>"};
@group(0) @binding(1) var<uniform> u: U;
fn px(p: vec2i) -> vec3f {
  let q = clamp(p, vec2i(0), vec2i(u.size.xy) - vec2i(1));
  ${external ? "return textureLoad(src, vec2u(q)).rgb;" : "return textureLoad(src, q, 0).rgb;"}
}
fn yuv(c: vec3f) -> vec3f { return mat3x3f(u.c0.xyz, u.c1.xyz, u.c2.xyz) * c + u.off.xyz; }
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  return vec4f(f32((i << 1u) & 2u) * 2.0 - 1.0, f32(i & 2u) * 2.0 - 1.0, 0.0, 1.0);
}
@fragment fn luma(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  return vec4f(yuv(px(vec2i(pos.xy))).x, 0.0, 0.0, 1.0);
}
@fragment fn chroma(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  let p = vec2i(pos.xy) * 2;
  let v = yuv((px(p) + px(p + vec2i(1, 0)) + px(p + vec2i(0, 1)) + px(p + vec2i(1, 1))) * 0.25);
  return vec4f(v.y, v.z, 0.0, 1.0);
}`;
}

function vdecPipeline(external, entry, format) {
  const key = (external ? "ext/" : "2d/") + entry + "/" + format;
  let p = V.pipelines.get(key);
  if (p)
    return p;
  let layout = V.layouts.get(external);
  if (!layout) {
    layout = device.createBindGroupLayout({ entries: [
      external ? { binding: 0, visibility: GPUShaderStage.FRAGMENT, externalTexture: {} }
               : { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "uniform" } }] });
    V.layouts.set(external, layout);
  }
  let module = V.pipelines.get(external ? "ext" : "2d");
  if (!module) {
    module = device.createShaderModule({ code: vdecWgsl(external) });
    V.pipelines.set(external ? "ext" : "2d", module);
  }
  p = device.createRenderPipeline({
    layout: device.createPipelineLayout({ bindGroupLayouts: [layout] }),
    vertex: { module, entryPoint: "vs" },
    fragment: { module, entryPoint: entry, targets: [{ format }] },
    primitive: { topology: "triangle-list" },
  });
  V.pipelines.set(key, p);
  return p;
}

// The surface's texture for resource `res`: a GPUTexture, BUSY (the ring is mid-record: again after the next drain)
// or null (no front end has it).
function vdecGrab(res) {
  const p = M._rvgpu_present_grab(res);
  if (!p)
    return null;
  const words = M.HEAPU32;
  if (!words[p >> 2])
    return words[(p >> 2) + 3] === PRESENT_BUSY ? BUSY : null;
  return M.rvgpuJsObject(words[p >> 2]) || null;
}

// The frame as something the shaders read: [external or 2d, the binding's resource, the counter]. A browser that will
// not import the frame as an external texture gets the RGBA copy from then on (V.noExternal).
function vdecSource(frame) {
  const w = frame.displayWidth, h = frame.displayHeight;
  if (!V.noExternal) {
    try {
      return [true, device.importExternalTexture({ source: frame }), "vdecExternal"];
    } catch (e) {
      V.noExternal = true;
      vlog("importExternalTexture: " + String(e && e.message || e) + ": the frames are copied to an RGBA texture instead");
    }
  }
  if (!V.rgba || V.rgba.width !== w || V.rgba.height !== h) {
    if (V.rgba)
      V.rgba.destroy();
    V.rgba = device.createTexture({ size: [w, h], format: "rgba8unorm",
                                    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
  }
  device.queue.copyExternalImageToTexture({ source: frame, flipY: false }, { texture: V.rgba, premultipliedAlpha: false }, [w, h]);
  return [false, V.rgba.createView(), "vdecRgba"];
}

// Shader path: the frame into the two textures. Throws on anything it cannot do (the caller then writes the bytes).
function vdecShade(frame, ty, tuv, w, h) {
  const [external, resource, counter] = vdecSource(frame);
  if (!V.ubo)
    V.ubo = device.createBuffer({ size: 80, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const { m, off } = vdecMatrix(frame.colorSpace);
  const u = new Float32Array(20);
  for (let c = 0; c < 3; c++)
    u.set(m.subarray(3 * c, 3 * c + 3), 4 * c);
  u.set(off, 12);
  u[16] = frame.displayWidth;
  u[17] = frame.displayHeight;
  device.queue.writeBuffer(V.ubo, 0, u);
  const enc = device.createCommandEncoder();
  for (const [tex, entry, vw, vh] of [[ty, "luma", w, h], [tuv, "chroma", (w + 1) >> 1, (h + 1) >> 1]]) {
    const pipeline = vdecPipeline(external, entry, tex.format);
    const pass = enc.beginRenderPass({ colorAttachments: [{
      view: tex.createView({ dimension: "2d", baseMipLevel: 0, mipLevelCount: 1, baseArrayLayer: 0, arrayLayerCount: 1 }),
      loadOp: "load", storeOp: "store" }] });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
      { binding: 0, resource }, { binding: 1, resource: { buffer: V.ubo } }] }));
    pass.setViewport(0, 0, vw, vh, 0, 1);
    pass.setScissorRect(0, 0, vw, vh);
    pass.draw(3);
    pass.end();
  }
  device.queue.submit([enc.finish()]);
  P.stats[counter]++;
  return counter;
}

// The planes' bytes as the textures hold them: luma w x h, chroma (w/2) x (h/2) interleaved. Async (copyTo).
async function vdecBytes(frame, w, h) {
  const rect = { x: 0, y: 0, width: w, height: h };
  const cw = w / 2, ch = h / 2;
  if (frame.format === "NV12") {
    const b = new Uint8Array(w * h + 2 * cw * ch);
    await frame.copyTo(b, { rect, layout: [{ offset: 0, stride: w }, { offset: w * h, stride: w }] });
    return { y: b.subarray(0, w * h), uv: b.subarray(w * h) };
  }
  const b = new Uint8Array(w * h * 3 / 2);
  await frame.copyTo(b, { rect, layout: [{ offset: 0, stride: w }, { offset: w * h, stride: cw },
                                          { offset: w * h + cw * ch, stride: cw }] });
  const uv = new Uint8Array(2 * cw * ch);
  for (let i = 0; i < cw * ch; i++) {
    uv[2 * i] = b[w * h + i];
    uv[2 * i + 1] = b[w * h + cw * ch + i];
  }
  return { y: b.subarray(0, w * h), uv };
}

function vdecWrite(planes, ty, tuv, w, h) {
  const cw = w / 2, ch = h / 2;
  device.queue.writeTexture({ texture: ty }, planes.y, { bytesPerRow: w }, { width: w, height: h });
  device.queue.writeTexture({ texture: tuv }, planes.uv, { bytesPerRow: 2 * cw }, { width: cw, height: ch });
  P.stats.vdecExact++;
}

// One frame. "again" when a drain began meanwhile (the exact path waits for copyTo) or the ring is busy: it stays first in
// the queue and goes after the next drain.
async function vdecOne(m) {
  const st = P.stats, frame = m.frame, t0 = performance.now();
  const dw = frame.displayWidth, dh = frame.displayHeight;
  const w = Math.min(m.w || dw, dw), h = Math.min(m.h || dh, dh);
  let planes = m.planes || null;
  const visible = frame.visibleRect ? frame.visibleRect.width === dw && frame.visibleRect.height === dh : true;
  const exact = m.exact || !visible || V.noShader;
  if (exact && !planes)
    planes = m.planes = await vdecBytes(frame, w & ~1, h & ~1);
  if (P.draining)
    return "again";
  const ty = vdecGrab(m.res[0]), tuv = vdecGrab(m.res[1]);
  if (ty === BUSY || tuv === BUSY)
    return "again";
  if (!ty || !tuv) {
    st.vdecSkipped++;
    st.vdecError = "surface resources " + m.res + " have no textures";
    vlog(st.vdecError);
    return "skipped";
  }
  if (ty.format !== "r8unorm" || tuv.format !== "rg8unorm") {
    st.vdecSkipped++;
    st.vdecError = "surface textures are " + ty.format + " + " + tuv.format + ", not r8unorm + rg8unorm";
    vlog(st.vdecError);
    return "skipped";
  }
  let path = "";
  if (!planes && (ty.usage & GPUTextureUsage.RENDER_ATTACHMENT) && (tuv.usage & GPUTextureUsage.RENDER_ATTACHMENT)) {
    try {
      path = vdecShade(frame, ty, tuv, w, h);
    } catch (e) {
      V.noShader = true;
      vlog("drawing with a shader failed (" + String(e && e.message || e) + "): the planes are copied instead");
    }
  }
  if (!path) {
    if (!planes)
      planes = m.planes = await vdecBytes(frame, w & ~1, h & ~1);
    if (P.draining)
      return "again";
    vdecWrite(planes, ty, tuv, w & ~1, h & ~1);
    path = "vdecExact";
  }
  st.vdecDrawn++;
  st.vdecPath = path.replace("vdec", "").toLowerCase();
  st.vdecLast = w + "x" + h + " " + (frame.format || "") + " seq " + m.seq;
  st.vdecLastMs = performance.now() - t0;
  st.vdecMs += st.vdecLastMs;
  return "drawn";
}

// The guest's wait ends (its driver reads the ready word at `ready`, a byte address in the machine's memory).
function vdecAck(m) {
  try { m.frame.close(); } catch (e) { /* (already closed) */ }
  if (V.memory)
    Atomics.store(new Int32Array(V.memory.buffer), Math.floor(m.ready / 4), m.seq | 0);
}

async function vdecPump() {
  if (V.busy)
    return;
  V.busy = true;
  try {
    while (V.queue.length && !P.draining) {
      const m = V.queue[0];
      let r = "error";
      try {
        r = await vdecOne(m);
      } catch (e) {
        P.stats.vdecErrors++;
        P.stats.vdecError = String(e && e.message || e);
        vlog("frame " + m.seq + ": " + P.stats.vdecError);
      }
      if (r === "again")
        break;
      V.queue.shift();
      vdecAck(m);
    }
  } finally {
    V.busy = false;
  }
  // (a drain that began while a frame was copying leaves it for the loop's next turn; the ring's busy: shortly)
  if (V.queue.length && !P.draining)
    setTimeout(vdecPump, 1);
}

// A frame from the decoder's Worker: queued, and the ring loop woken so that it drains first (the ring up to date), then
// draws it. Before the executor is up there is nothing to draw into: the frame is given back, the guest told.
function vdecReceive(m) {
  P.stats.vdecFrames++;
  if (!M || !P.bell) {
    P.stats.vdecSkipped++;
    vdecAck(m);
    return;
  }
  V.queue.push(m);
  Atomics.notify(P.bell, 0);
}

function vdecAttach(port) {
  V.port = port;
  port.onmessage = (event) => {
    const m = event.data;
    if (m && m.type === "vdec-frame")
      vdecReceive(m);
  };
}

self.onmessage = (event) => {
  const m = event.data;
  if (!m) return;
  if (m.type === "vfront-replay") {
    vfrontReplay(m).catch((e) => self.postMessage({ type: "vfront-replay", ok: false, msg: String(e && e.stack || e),
                                                    errors }));
    return;
  }
  if (m.type === "smoke") {
    smoke(m).catch((e) => self.postMessage({ type: "smoke", ok: false, msg: String(e && e.stack || e), errors }));
    return;
  }
  if (m.type === "vdec-port") {
    vdecAttach(m.port);
    return;
  }
  if (m.type === "start") {
    V.memory = m.memory;
    makeDevice().then((d) => { device = d; }, (e) => {
      // (no WebGPU here: say so before anything touches the ring, and the page starts vrend instead; the video
      // decoder's port, if it came, goes with it: vrend draws its frames)
      const port = V.port;
      V.port = null;
      if (port)
        port.onmessage = null;
      self.postMessage({ type: "ready", exec: "webgpu", fallback: true, errors: [String(e && e.message || e)],
                         vdecPort: port }, port ? [port] : []);
      return Promise.reject(null);
    }).then(() => boot(m.base, m.memory)).then(async () => {
      const caps = m.caps || { at: 0, size: 0 };
      if (M._rvgpu_ring_attach(BigInt(m.ring), 0, BigInt(caps.at), caps.size) !== 0)
        throw new Error("no ring at " + m.ring);
      // Caps and exec_state 1 are written by the front ends: vfront (virgl caps from zink on wgvk) starts on
      // the ring here when it is linked in, vkr (venus: its capset) too; without one gpuserver keeps waiting.
      let renderer = "wgvk on " + adapterInfo;
      if (M._vfront_rvgpu_start) {
        if ((await M._vfront_rvgpu_start()) !== 0)
          throw new Error("vfront did not start (zink found no device on wgvk)");
        renderer = "vfront: zink on wgvk on " + adapterInfo;
      }
      // (for the page to read what the executor is doing when this thread stops answering)
      if (M._rvgpu_debug_where && M._rvgpu_debug_words_at) {
        self.postMessage({ type: "debug", heap: M.HEAPU8.buffer, where: M._rvgpu_debug_where(),
                           words: M._rvgpu_debug_words_at() });
      }
      if (M._rvgpu_vkr_page_start) {
        const t0 = performance.now();
        const r = M._rvgpu_vkr_page_start();
        log("rvgpu: vkr (venus) " + (r === 0 ? "started" : "failed to start") + " in " + (performance.now() - t0).toFixed(1) + " ms");
        if (r === 0)
          renderer += " + vkr";
      }
      const capsLen = new Int32Array(m.memory.buffer, m.ring + 84, 1)[0];
      P.memory = m.memory;
      P.ring = m.ring;
      self.postMessage({ type: "ready", renderer, exec: "webgpu", capsLen, errors, build: stamp });
      setInterval(() => self.postMessage({ type: "stats", stats: P.stats }), 1000);
      return run(m.memory, m.ring);
    }).catch((e) => e && self.postMessage({ type: "ready", exec: "webgpu", errors: [String(e && e.stack || e)] }));
    return;
  }
  if (m.type === "table") {
    // The window server's surface table (wlserver/README.md), watched every 2 ms: a GPU window's new frame is
    // drawn and on its way to the page as soon as its slot names it and `done` has passed it.
    P.table = m;
    if (!P.timer)
      P.timer = setInterval(watchTable, 2);
    return;
  }
  if (m.type === "grab") {
    // (display.js asks for a resource whose newest frame it has not got: the table watch has usually sent it)
    if (M && P.memory)
      present(m.ctx, m.res, m.present);
    return;
  }
  if (m.type === "imported") {
    const f = P.inflight.get(m.res);
    if (f && f.frame === m.frame) {
      P.inflight.delete(m.res);
      P.stats.roundMs += performance.now() - f.t;
      P.stats.rounds++;
    }
    const want = P.wanted.get(m.res);
    if (want) {
      P.wanted.delete(m.res);
      present(want.ctx, m.res, want.frame);
    }
    return;
  }
  // ("shown" {serial}: vrend's PRESENT records; nothing here)
};
