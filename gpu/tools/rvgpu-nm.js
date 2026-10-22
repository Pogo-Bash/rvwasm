// The real GPU executor under tools/node-machine.js (--hook gpu/tools/rvgpu-nm.js): what the page's gpu-exec.js does
// for ?exec=webgpu, with Node as the host. Where gpu/tools/gpu-stub.js answers gpuserver's ring with a colour,
// this starts tools/www/rvgpu-worker.js (unmodified: vfront on zink on wgvk, and vkr, rvgpu.wasm) in a Node Worker on
// Dawn's Node bindings (gpu/tools/rvgpu-nm-worker.js over VK-02's worker-shim.js), hands it the machine's memory and
// the ring, and drives it with the same messages as the page: "start" with memory/ring/caps, then the window
// server's surface table ("table"), and "imported" for every "present" it gets back.
//
// Needs Node 26 (npm package `node`: gpu/webgpu/tests/dawn-node, `npm ci`): JSPI for rvgpu.wasm, and node-machine.js
// runs on it too (memory64 without the flag). tools/gpu-nm.sh does all of it; this file is its hook.
//
//   RVGPU_DIR=DIR         the directory with rvgpu-worker.js and rvgpu/rvgpu.{js,wasm} (default build/rvgpu-page);
//                         rvgpu-worker.js in it is replaced by this tree's tools/www/ copy
//   RVGPU_NM_CHAIN=FILE   also load that hook (default tools/wl-dump.js: acknowledges the windows' frames, PNGs of the
//                         windows; "none": nothing)
//   RVGPU_NM_SHOTS=N      write at most N of the executor's presents (stage B windows) as PNGs to $RVGPU_NM_DIR (default 8)
//   RVGPU_NM_DIR=DIR      where (default $WL_DUMP_DIR or build/shots)
//   RVGPU_NM_LOG=1        every line the Worker logs (otherwise: the build stamp, errors, vkr, device lost)
//   RVGPU_NM_LAZY=1         start the executor the way VK-15's gpu-exec.js does: exec_state 4 until gpuserver says `want`
//   RVGPU_NM_DEVICE_LOSS=S  destroy the GPU device S seconds after the executor is up (VK-13: the loss path)
//   DAWN_OPTS=a=b,c=d     dawn.node options (backend=vulkan, adapter=...)
//
// Video (VK-14): `os.rvgpuNm.vdecPort(port)` hands the Worker the decoder's MessagePort ("vdec-port", as gpu-exec.js does
// for the page's vdec-worker.js); gpu/tools/vdec-stub.js (VDEC_STUB_GPU=executor, chained below with RVGPU_NM_CHAIN) uses it
// to send synthetic frames to be drawn into the guest's surfaces. The Worker's vdec* counters are in the summary (`vdec`).
//
// It prints ("[rvgpu-nm] ...", standard error): the stamp, the adapter, caps length, how long the executor took to
// answer; at the end a summary line "[rvgpu-nm] summary {...}" (JSON: stamp, adapter, presents, ring counters,
// errors) that tools/gpu-nm.sh reads.
"use strict";

const { Worker } = require("worker_threads");
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");
// (An address of the machine's memory as the index of its 32-bit word: exact past 2 and 4 GiB, which `>> 2` is not.)
const ix32 = (address) => Math.floor(address / 4);

const ROOT = path.resolve(__dirname, "../..");
const R = { HEAD: 24, DOORBELL: 28, SUBMITTED: 32, TAIL: 64, DONE: 68, EXEC_HEARTBEAT: 72, EXEC_STATE: 76, CAPS_LEN: 84 };

const say = (text) => process.stderr.write("[rvgpu-nm] " + text + "\n");

const crcTable = new Int32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  crcTable[n] = c;
}
const crc32 = (buf) => {
  let c = -1;
  for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
};
const pngRGBA = (rgba, w, h) => {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr),
                        chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
};

module.exports = (os, opts) => {
  const chainFile = process.env.RVGPU_NM_CHAIN === undefined ? path.join(ROOT, "tools/wl-dump.js") : process.env.RVGPU_NM_CHAIN;
  const chained = chainFile && chainFile !== "none" ? require(path.resolve(chainFile))(os, opts) : null;
  const dir = path.resolve(process.env.RVGPU_DIR || path.join(ROOT, "build/rvgpu-page"));
  const shotsDir = process.env.RVGPU_NM_DIR || process.env.WL_DUMP_DIR || path.join(ROOT, "build/shots");
  const maxShots = process.env.RVGPU_NM_SHOTS === undefined ? 8 : Number(process.env.RVGPU_NM_SHOTS);
  const verbose = !!process.env.RVGPU_NM_LOG;
  for (const f of ["rvgpu/rvgpu.js", "rvgpu/rvgpu.wasm"]) {
    if (!fs.existsSync(path.join(dir, f))) {
      say("no " + path.join(dir, f) + " (RVGPU_DIR; tools/gpu-nm.sh --fetch-rvgpu BUILD copies one from the build VM)");
      process.exit(2);
    }
  }
  fs.copyFileSync(path.join(ROOT, "tools/www/rvgpu-worker.js"), path.join(dir, "rvgpu-worker.js"));

  const summary = { exec: "webgpu", presents: 0, imported: 0, shots: 0, errors: [], logs: 0 };
  let ring = 0, caps = null, pid = 0, started = false, shimReady = false, table = null, lastStats = null;
  const t0 = Date.now();
  const w = new Worker(path.join(__dirname, "rvgpu-nm-worker.js"), {
    workerData: { dir, dawn: (process.env.DAWN_OPTS || "").split(",").filter(Boolean) },
    execArgv: [], stdout: false, stderr: false,
  });
  w.on("error", (e) => {
    say("the executor's Worker failed: " + (e && e.stack || e));
    summary.errors.push("worker: " + String(e && e.message || e));
  });
  let ending = false;
  w.on("exit", (code) => {
    if (code && !ending) say("the executor's Worker exited with " + code);
  });

  // (for gpu/tools/vdec-stub.js: the page's rvGpu.vdecPort)
  os.rvgpuNm = { vdecPort: (port) => w.postMessage({ type: "vdec-port", port }, [port]) };

  const start = () => {
    if (started || !shimReady || !ring) return;
    started = true;
    w.postMessage({ type: "start", memory: os.memory, ring, exec: "webgpu", caps, base: dir + "/" });
    if (table) w.postMessage({ type: "table", ...table, gpuPid: pid });
  };

  let lazyWantAt = 0;
  const gpuChannel = new BroadcastChannel("rv-gpu");
  gpuChannel.onmessage = (event) => {
    const m = event.data;
    if (!m || m.type !== "gpu" || (m.version !== 1 && m.version !== 2) || ring) return;
    // VK-15's lazy start, as gpu-exec.js does it (RVGPU_NM_LAZY=1): while gpuserver says `lazy` and has not said `want`
    // (no program has connected yet), promise an executor with exec_state 4 and start nothing; the announcement with
    // `want: true` (asked for every 2 s below) starts it.
    if (process.env.RVGPU_NM_LAZY && m.lazy && !m.want) {
      if (!summary.lazy) say("lazy: exec_state 4 written, the executor waits for the first program (gpuserver's `want`)");
      summary.lazy = "waiting";
      try {
        Atomics.store(new Int32Array(os.memory.buffer), ix32(m.ring + R.EXEC_STATE), 4);
      } catch (e) {
        say("could not write exec_state 4: " + e);
      }
      return;
    }
    if (summary.lazy === "waiting") {
      summary.lazy = "started";
      lazyWantAt = Date.now();
      say("lazy: gpuserver says `want` (a program has connected): starting the executor");
    }
    ring = m.ring;
    pid = m.pid;
    caps = m.caps ? { at: m.caps, size: m.capsSize } : null;
    say("gpuserver's ring at " + ring + " (pid " + pid + ")" + (caps ? ", caps at " + caps.at : ""));
    start();
  };
  // (as gpu-exec.js: asked again every 2 s until a ring is taken)
  const ask = setInterval(() => (ring ? clearInterval(ask) : gpuChannel.postMessage({ type: "gpu?" })), 2000);
  gpuChannel.postMessage({ type: "gpu?" });
  // The window server's surface table, for the Worker to watch GPU windows (slot format 2) itself.
  const displayChannel = new BroadcastChannel("rv-display");
  displayChannel.onmessage = (event) => {
    const t = event.data;
    if (!t || t.type !== "table" || (table && table.address === t.address)) return;
    table = { address: t.address, slots: t.slots, slotSize: t.slotSize, header: t.header };
    if (started) w.postMessage({ type: "table", ...table, gpuPid: pid });
  };

  w.on("message", (m) => {
    switch (m.type) {
      case "shim-ready":
        shimReady = true;
        say("the Worker is up on Dawn after " + (Date.now() - t0) + " ms");
        start();
        break;
      case "log":
        summary.logs++;
        if (verbose || /build |error|lost|failed|WebGPU|vkr|fallback|trap|ring drain/i.test(m.text)) say("worker: " + m.text);
        if (/^rvgpu: build /.test(m.text)) summary.stamp = m.text.slice("rvgpu: build ".length);
        if (/error|failed|lost/i.test(m.text) && summary.errors.length < 20) summary.errors.push(m.text);
        // as gpu-exec.js deviceLost (VK-13): exec_state 2 into the ring, the Worker stopped; gpuserver then drops .ready
        // and writes rvgpu-0.none, so programs started from now on draw in software
        if (/^WebGPU device lost:/.test(m.text) && !summary.deviceLost) {
          summary.deviceLost = m.text;
          try {
            Atomics.store(new Int32Array(os.memory.buffer), ix32(ring + R.EXEC_STATE), 2);
          } catch (e) {
            say("could not write exec_state 2: " + e);
          }
          say("device lost: exec_state 2 written, Worker stopped");
          w.terminate();
        }
        break;
      case "ready":
        if (m.fallback || (m.errors && m.errors.length && !m.renderer)) {
          say("the executor did not start: " + (m.errors || []).join("; "));
          summary.errors.push("start: " + (m.errors || []).join("; "));
          summary.fallback = true;
          break;
        }
        summary.renderer = m.renderer;
        summary.capsLen = m.capsLen;
        summary.build = m.build;
        summary.readyMs = Date.now() - t0;
        if (lazyWantAt) summary.lazyStartMs = Date.now() - lazyWantAt;
        say("executor ready after " + summary.readyMs + " ms: " + m.renderer + "; " + m.capsLen + " bytes of caps");
        if (process.env.RVGPU_NM_DEVICE_LOSS) {
          setTimeout(() => {
            say("RVGPU_NM_DEVICE_LOSS: destroying the device");
            w.postMessage({ type: "nm-destroy-device" });
          }, Number(process.env.RVGPU_NM_DEVICE_LOSS) * 1000);
        }
        break;
      case "stats":
        lastStats = m.stats;
        break;
      case "present": {
        summary.presents++;
        w.postMessage({ type: "imported", res: m.res, frame: m.frame });	// (the page took it: the next may come)
        summary.imported++;
        const b = m.bitmap;
        if (b && b.rgba && summary.shots < maxShots) {
          fs.mkdirSync(shotsDir, { recursive: true });
          const file = path.join(shotsDir, "gpu-res" + m.res + "-" + m.frame + ".png");
          fs.writeFileSync(file, pngRGBA(b.rgba, b.width, b.height));
          summary.shots++;
          say("present res " + m.res + " frame " + m.frame + " " + b.width + "x" + b.height + " " + b.format + " -> " + file);
        }
        summary.lastPresent = b ? b.width + "x" + b.height : "";
        break;
      }
      case "debug":
        break;
      case "nm-waits":
        summary.waits = m.waits;	// wgvk_wait calls = JSPI suspensions
        break;
      default:
        if (verbose) say("worker message " + JSON.stringify(m).slice(0, 200));
    }
  });

  const ringWords = () => {
    if (!ring || !os.memory) return null;
    try {
      const i = new Int32Array(os.memory.buffer);
      const at = (o) => Atomics.load(i, ix32(ring + o)) >>> 0;
      return { head: at(R.HEAD), tail: at(R.TAIL), done: at(R.DONE), submitted: at(R.SUBMITTED), heartbeat: at(R.EXEC_HEARTBEAT),
               execState: at(R.EXEC_STATE), capsLen: at(R.CAPS_LEN) };
    } catch (e) {
      return null;
    }
  };

  return {
    done: async () => {
      // (the Worker's counters as they are now, not as of the last second)
      await new Promise((resolve) => {
        const t = setTimeout(resolve, 1500);
        w.on("message", (m) => { if (m.type === "nm-stats-done") { clearTimeout(t); resolve(); } });
        w.postMessage({ type: "nm-stats" });
      });
      summary.ring = ringWords();
      summary.presentStats = lastStats;
      if (lastStats && lastStats.vdecFrames !== undefined) {
        summary.vdec = {};
        for (const k of Object.keys(lastStats)) if (/^vdec/.test(k)) summary.vdec[k] = lastStats[k];
      }
      say("summary " + JSON.stringify(summary));
      if (chained && typeof chained.done === "function") await chained.done();
      ending = true;
      await w.terminate();
    },
  };
};
