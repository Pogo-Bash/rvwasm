#!/usr/bin/env node
"use strict";
// VK-14: the video-frame drawing of tools/www/rvgpu-worker.js on a real GPU device (Dawn's Node bindings), without a machine,
// a ring or rvgpu.wasm. The Worker's own source is run (in a vm context, as the shim runs it), its `M` (the module) replaced by a
// stand-in that hands out two textures for two resource ids as rvgpu_present_grab does, and frames made of the test card
// (gpu/tools/synth-frame.js) go through vdecReceive/vdecPump exactly as the decoder's port delivers them. The textures are read
// back and compared with the card's own Y and CbCr planes:
//   - the shader path (a frame copied into an RGBA texture here, because dawn.node cannot import a VideoFrame as an external
//     texture: the page's importExternalTexture is the one part not run) in full and limited range and with the BT.601 matrix;
//   - the exact path (copyTo + writeTexture), which must be bit for bit;
//   - a surface larger than the picture (the texels outside the picture stay as they were), a chroma plane of odd size;
//   - the ready word written for a frame whatever happened (no textures for the resource, wrong texture format).
//   cd gpu/webgpu/tests/dawn-node && ./node_modules/.bin/node ../../../tools/vdec-draw-test.js     (Node 26 and `npm ci` there)
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const ROOT = path.resolve(__dirname, "../..");
const { create, globals } = require(path.join(ROOT, "gpu/webgpu/tests/dawn-node/node_modules/webgpu"));
const { testCard, SynthFrame } = require("./synth-frame.js");
Object.assign(globalThis, globals);

let failures = 0, checks = 0;
const ok = (name, good, detail) => {
  checks++;
  if (!good) failures++;
  console.log((good ? "  ok   " : "  FAIL ") + name + (detail ? " (" + detail + ")" : ""));
};

(async () => {
  const gpu = create([]);
  const adapter = await gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const errors = [];
  device.addEventListener("uncapturederror", (e) => errors.push(e.error.message));
  // (as gpu/tools/rvgpu-nm-worker.js: dawn.node takes no VideoFrame, the stand-in frame's RGB goes in by writeTexture)
  device.importExternalTexture = () => { throw new TypeError("dawn.node takes no VideoFrame"); };
  device.queue.copyExternalImageToTexture = (src, dst) => {
    const f = src.source;
    device.queue.writeTexture({ texture: dst.texture }, f.rgba(), { bytesPerRow: f.displayWidth * 4 }, { width: f.displayWidth, height: f.displayHeight });
  };
  console.log("adapter: " + (adapter.info && adapter.info.description));

  // ---- the Worker's script, as the shim runs it
  const logs = [];
  const sandbox = { self: null, console, Atomics, SharedArrayBuffer, Int32Array, performance, setTimeout, clearTimeout, setInterval, clearInterval,
                    navigator: { gpu }, ...globals, WorkerGlobalScope: class {} };
  sandbox.self = sandbox;
  sandbox.postMessage = (m) => { if (m && m.type === "log") logs.push(m.text); };
  const ctx = vm.createContext(sandbox);
  const file = path.join(ROOT, "tools/www/rvgpu-worker.js");
  vm.runInContext(fs.readFileSync(file, "utf8"), ctx, { filename: file });
  const run = (code) => vm.runInContext(code, ctx);

  // ---- the stand-ins: the module (two textures by resource id) and the machine's memory (the ready words)
  const mem = new SharedArrayBuffer(1 << 16);
  const heap = new Uint32Array(16);	// (M.HEAPU32: the grab's four words at byte 16)
  const textures = new Map();
  const M = {
    HEAPU32: heap,
    _rvgpu_present_grab(res) {
      const t = textures.get(res);
      if (!t) return 0;
      heap[4] = res;	// (the "handle": rvgpuJsObject gives the texture back)
      heap[5] = t.width;
      heap[6] = t.height;
      heap[7] = 1;
      return 16;
    },
    rvgpuJsObject(h) { return textures.get(h); },
  };
  sandbox.__device = device;
  sandbox.__M = M;
  sandbox.__mem = { buffer: mem };	// (the machine's WebAssembly.Memory)
  sandbox.__bell = new Int32Array(mem, 0, 1);
  run("device = __device; M = __M; V.memory = __mem; P.bell = __bell;");

  const USAGE = GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST | GPUTextureUsage.TEXTURE_BINDING;
  const makeSurface = (w, h, resY, resUV, fill) => {
    const ty = device.createTexture({ size: [w, h], format: "r8unorm", usage: USAGE });
    const tuv = device.createTexture({ size: [w >> 1, h >> 1], format: "rg8unorm", usage: USAGE });
    if (fill !== undefined) {
      device.queue.writeTexture({ texture: ty }, new Uint8Array(w * h).fill(fill), { bytesPerRow: w }, { width: w, height: h });
      device.queue.writeTexture({ texture: tuv }, new Uint8Array(w * h / 2).fill(fill), { bytesPerRow: w }, { width: w >> 1, height: h >> 1 });
    }
    textures.set(resY, ty);
    textures.set(resUV, tuv);
    return { ty, tuv, w, h };
  };
  const readTexture = async (tex, bytesPerTexel) => {
    const bpr = Math.ceil(tex.width * bytesPerTexel / 256) * 256;
    const buf = device.createBuffer({ size: bpr * tex.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: bpr }, [tex.width, tex.height]);
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buf.getMappedRange());
    const out = new Uint8Array(tex.width * tex.height * bytesPerTexel);
    for (let y = 0; y < tex.height; y++) out.set(src.subarray(y * bpr, y * bpr + tex.width * bytesPerTexel), y * tex.width * bytesPerTexel);
    buf.unmap();
    buf.destroy();
    return out;
  };
  const wordAt = (addr) => Atomics.load(new Int32Array(mem), Math.floor(addr / 4));
  let nextReady = 1024, nextSeq = 1;
  // One frame through the Worker's queue; resolves when its ready word has been written (or after 3 s: then it failed).
  const send = async (frame, res, w, h, exact) => {
    const ready = nextReady, seq = nextSeq++;
    nextReady += 4;
    sandbox.__m = { type: "vdec-frame", frame, res, w, h, ready, seq, exact };
    run("vdecReceive(__m); vdecPump();");
    for (let i = 0; i < 600 && wordAt(ready) !== seq; i++) await new Promise((r) => setTimeout(r, 5));
    return wordAt(ready) === seq;
  };
  const frameOf = (w, h, seq, colorSpace, dw, dh) => {
    const { y, uv } = testCard(w, h, seq);
    return new SynthFrame({ timestamp: seq, format: "NV12", codedWidth: w, codedHeight: h, displayWidth: dw || w, displayHeight: dh || h,
                            colorSpace: colorSpace || { primaries: "bt709", transfer: "bt709", matrix: "bt709", fullRange: true }, y, uv });
  };
  // the card's own planes, as the surface holds them (luma w x h; chroma w/2 x h/2 interleaved)
  const planes = (f, w, h) => {
    const y = new Uint8Array(w * h), uv = new Uint8Array(w * h / 2);
    for (let r = 0; r < h; r++) y.set(f.y.subarray(r * f.codedWidth, r * f.codedWidth + w), r * w);
    for (let r = 0; r < h / 2; r++) uv.set(f.uv.subarray(r * f.codedWidth, r * f.codedWidth + w), r * w);
    return { y, uv };
  };
  const diff = (a, b) => {
    let max = 0, sum = 0, over = 0;
    for (let i = 0; i < a.length; i++) {
      const d = Math.abs(a[i] - b[i]);
      max = Math.max(max, d);
      sum += d;
      if (d > 3) over++;
    }
    return { max, mean: sum / a.length, over };
  };

  // 1. the shader path, 640x368 surface, picture 640x360 (a surface rounded up to 16 rows, as the driver makes them), BT.709 full range
  {
    const S = makeSurface(640, 368, 1, 2, 77);
    const f = frameOf(640, 360, 3);
    const done = await send(f, [1, 2], 640, 360, false);
    ok("shader path: the ready word is written", done);
    const st = run("P.stats");
    ok("shader path: counted as drawn from an RGBA copy (importExternalTexture is not run under Node)", st.vdecDrawn === 1 && st.vdecRgba === 1 && st.vdecExact === 0, JSON.stringify({ drawn: st.vdecDrawn, rgba: st.vdecRgba, path: st.vdecPath }));
    const y = await readTexture(S.ty, 1), uv = await readTexture(S.tuv, 2);
    const want = planes(f, 640, 360);
    const yd = diff(y.subarray(0, 640 * 360), want.y), uvd = diff(uv.subarray(0, 640 * 180), want.uv);
    // (the RGB in between is 8 bit: Y within 2, Cb and Cr within 3 of the original)
    ok("shader path: luma within 2 codes of the picture's", yd.max <= 2, "max " + yd.max + ", mean " + yd.mean.toFixed(3));
    ok("shader path: chroma within 3 codes of the picture's", uvd.max <= 3, "max " + uvd.max + ", mean " + uvd.mean.toFixed(3));
    ok("shader path: rows below the picture (the surface's extra 8 rows) untouched", y.subarray(640 * 360).every((v) => v === 77) && uv.subarray(640 * 180).every((v) => v === 77));
    // the orientation: the bars (top of the picture) are in row 0 of the texture
    ok("shader path: the picture's top row is the texture's row 0 (white bar's luma at the top left, black bar's at the top right)", y[2] > 250 && y[640 - 3] < 5, y[2] + " / " + y[637]);
    // (the green bar, x = 3 * 80 + 40: Cb 29 and Cr 12 of 255 in BT.709 full range)
    const g = (3 * 80 + 40) & ~1;
    ok("shader path: the green bar's chroma is green's (Cb 29, Cr 12)", Math.abs(uv[g] - 29) <= 2 && Math.abs(uv[g + 1] - 12) <= 2, uv[g] + "," + uv[g + 1]);
  }

  // 2. limited range and BT.601: the matrix of the frame's colour space is the inverse of the RGB that was made from it
  for (const cs of [{ primaries: "bt709", transfer: "bt709", matrix: "bt709", fullRange: false }, { primaries: "smpte170m", transfer: "smpte170m", matrix: "smpte170m", fullRange: true },
                    { primaries: "smpte170m", transfer: "smpte170m", matrix: "bt470bg", fullRange: false }]) {
    const S = makeSurface(320, 192, 3, 4, 0);
    // (the card's planes are full-range BT.709 numbers; the frame says otherwise, so what RGB makes of them is a different
    // picture, but the Worker must give the same planes back: the round trip, not the picture, is what is compared)
    const f = frameOf(320, 192, 5, cs);
    const done = await send(f, [3, 4], 320, 192, false);
    const y = await readTexture(S.ty, 1), uv = await readTexture(S.tuv, 2);
    const want = planes(f, 320, 192);
    // (clipped RGB loses what is out of gamut: compare only the pixels whose RGB was inside 0..1)
    const rgba = f.rgba();
    let n = 0, worst = 0;
    for (let i = 0; i < 320 * 192; i++) {
      const o = i * 4;
      if ([rgba[o], rgba[o + 1], rgba[o + 2]].some((v) => v === 0 || v === 255)) continue;
      n++;
      worst = Math.max(worst, Math.abs(y[i] - want.y[i]));
    }
    ok("matrix " + cs.matrix + (cs.fullRange ? " full" : " limited") + ": ready, and luma within 3 codes where the RGB was not clipped (" + n + " pixels)", done && worst <= 3, "worst " + worst);
  }

  // 3. the exact path: bit for bit, and a surface the same size as the picture
  {
    const S = makeSurface(256, 128, 5, 6, 9);
    const f = frameOf(256, 128, 9);
    const done = await send(f, [5, 6], 256, 128, true);
    const y = await readTexture(S.ty, 1), uv = await readTexture(S.tuv, 2);
    const want = planes(f, 256, 128);
    ok("exact path: the ready word is written", done);
    ok("exact path: luma and chroma bit for bit", diff(y, want.y).max === 0 && diff(uv, want.uv).max === 0, "luma max " + diff(y, want.y).max + ", chroma max " + diff(uv, want.uv).max);
    ok("exact path: counted", run("P.stats.vdecExact") >= 1 && run("P.stats.vdecPath") === "exact", run("P.stats.vdecPath"));
  }

  // 4. a picture smaller than the surface: only its texels are written
  {
    const S = makeSurface(256, 128, 7, 8, 200);
    const f = frameOf(192, 96, 2);
    const done = await send(f, [7, 8], 192, 96, false);
    const y = await readTexture(S.ty, 1);
    let inside = 0, outside = 0;
    for (let r = 0; r < 128; r++) for (let x = 0; x < 256; x++) {
      const v = y[r * 256 + x];
      if (x < 192 && r < 96) { if (v !== 200) inside++; } else if (v === 200) outside++;
    }
    ok("smaller picture: written inside its rectangle, the rest of the surface untouched", done && inside > 0.9 * 192 * 96 && outside === 256 * 128 - 192 * 96, "inside " + inside + ", untouched " + outside);
  }

  // 5. things that go wrong still end the guest's wait
  {
    const f = frameOf(64, 64, 1);
    const done = await send(f, [99, 100], 64, 64, false);
    ok("no textures for the resources: the ready word is written all the same, counted skipped", done && run("P.stats.vdecSkipped") >= 1, run("P.stats.vdecError"));
    const bad = device.createTexture({ size: [64, 64], format: "rgba8unorm", usage: USAGE });
    textures.set(101, bad);
    textures.set(102, bad);
    const done2 = await send(frameOf(64, 64, 2), [101, 102], 64, 64, false);
    ok("textures of the wrong format: ready written, counted skipped", done2 && run("P.stats.vdecSkipped") >= 2, run("P.stats.vdecError"));
  }

  // 6. many frames in a row come out in order, every one acknowledged
  {
    const S = makeSurface(128, 64, 11, 12, 0);
    let all = true;
    const before = run("P.stats.vdecDrawn");
    for (let i = 0; i < 20; i++) all = (await send(frameOf(128, 64, 20 + i), [11, 12], 128, 64, false)) && all;
    ok("20 frames in a row: all acknowledged, all drawn", all && run("P.stats.vdecDrawn") - before === 20, run("P.stats.vdecDrawn") - before + " drawn");
    const f = frameOf(128, 64, 39);
    const y = await readTexture(S.ty, 1);
    // (the marker moves 8 px a picture: the last picture's marker is where picture 39 puts it)
    const mx = (39 * 8) % (128 - 32), row = Math.floor(64 * 0.8) + 4;
    ok("the last frame is what is in the texture (its marker at x = " + mx + ")", y[row * 128 + mx + 8] > 200 && (mx < 16 || y[row * 128 + mx - 8] < 40), y[row * 128 + mx + 8] + "");
    void f;
  }

  ok("no WebGPU validation errors", errors.length === 0, errors.slice(0, 2).join(" | "));
  const st = run("P.stats");
  console.log("stats: " + JSON.stringify(Object.fromEntries(Object.entries(st).filter(([k]) => /^vdec/.test(k)))));
  if (logs.length) console.log("worker log: " + logs.join(" | ").slice(0, 600));
  console.log(failures ? "FAIL " + failures + " of " + checks : "PASS " + checks + " checks");
  process.exit(failures ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
