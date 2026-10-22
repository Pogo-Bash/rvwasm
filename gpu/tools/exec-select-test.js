// Which GPU executor the page starts (VK-13), under Node: tools/www/gpu-exec.js on the page side with a fake window,
// document, navigator, fetch and Worker. Checks (1) the selection table (override > rv-site.json gpu_exec > vrend;
// no navigator.gpu, probe fail, D1 browser rule, no WebGL2), (2) the adapter probe, (3) the whole page flow: the
// announcement of the ring, the rv-site.json fetch, the Worker that is started and the fallback chain, (4) a lost
// WebGPU device: exec_state 2 in the ring, a notice, the Worker stopped.
//
//   node gpu/tools/exec-select-test.js
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

let failed = 0, count = 0;
const check = (name, ok, extra) => {
  count++;
  if (!ok) {
    failed++;
  }
  console.log((ok ? "ok   " : "FAIL ") + name + (ok || extra === undefined ? "" : "  (" + JSON.stringify(extra) + ")"));
};

// ---- the page's globals, one page per call
const source = fs.readFileSync(path.join(__dirname, "../../tools/www/gpu-exec.js"), "utf8");
let sel = null;
const page = ({ search = "", site, siteStatus = 200, hasGpu = true, adapter, userAgentData }) => {
  const workers = [], notices = [], channels = {};
  const memory = new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
  const fetched = [];
  const nav = { userAgentData };
  if (hasGpu) {
    nav.gpu = { requestAdapter: async () => adapter === undefined ? goodAdapter() : adapter };
  }
  class FakeWorker {
    constructor(url) {
      this.url = String(url).split("/").pop();
      this.messages = [];
      this.terminated = false;
      workers.push(this);
    }
    postMessage(m) {
      this.messages.push(m);
    }
    terminate() {
      this.terminated = true;
    }
  }
  const el = () => ({ style: {}, setAttribute() {}, set textContent(t) { notices.push(t); } });
  const ctx = {
    console: { log() {} },
    performance, Atomics, Int32Array, Uint8Array, Uint32Array, Map, Promise, URLSearchParams, WebAssembly, AbortSignal,
    setInterval: () => 1, clearInterval() {}, setTimeout, clearTimeout,
    location: { search },
    navigator: nav,
    document: { hidden: false, currentScript: { src: "https://x.test/desktop/gpu-exec.js" }, createElement: el,
                body: { appendChild() {} } },
    fetch: async (url) => {
      fetched.push(url);
      if (site === undefined) {
        throw new Error("offline");
      }
      return { ok: siteStatus === 200, json: async () => (typeof site === "string" ? JSON.parse(site) : site) };
    },
    Worker: FakeWorker,
  };
  ctx.globalThis = ctx;
  ctx.window = ctx;
  ctx.rvChannel = (name) => (channels[name] = channels[name] || { postMessage() {}, onmessage: null });
  ctx.linuxOs = { memory };
  ctx.rvDisplay = { gl: { getExtension: () => null, getParameter: () => "fake gl", RENDERER: 1 } };
  vm.createContext(ctx);
  vm.runInContext(source, ctx, { filename: "gpu-exec.js" });
  sel = ctx.rvGpuSelect;
  return { ctx, workers, notices, memory, fetched,
           announce: (extra = {}) => channels["rv-gpu"].onmessage({ data: { type: "gpu", version: 2, ring: 4096, pid: 7, ...extra } }) };
};
const goodAdapter = (over = {}) => ({
  isFallbackAdapter: false,
  features: new Set(["texture-compression-bc"]),
  limits: { maxBindGroups: 4, maxColorAttachments: 8, maxVertexBuffers: 8, maxVertexAttributes: 16,
            maxStorageBuffersPerShaderStage: 10, maxStorageBufferBindingSize: 2 ** 31 - 1, maxBufferSize: 2 ** 32,
            maxTextureDimension2D: 16384, maxStorageTexturesPerShaderStage: 8, ...over },
});
const chrome = { mobile: false, brands: [{ brand: "Not;A=Brand" }, { brand: "Chromium" }, { brand: "Google Chrome" }] };
const settle = () => new Promise((r) => setTimeout(r, 20));

(async () => {
  // ---- 1. the selection table (pure)
  page({});
  const S = (i) => sel.selectExec({ hasGL2: true, hasGpu: true, browserOk: true, probe: { ok: true }, ...i }).exec;
  check("table: nothing set -> vrend", S({}) === "vrend");
  check("table: site vrend -> vrend", S({ site: "vrend" }) === "vrend");
  check("table: site junk -> vrend", S({ site: "yes please" }) === "vrend");
  check("table: site webgpu, all pass -> webgpu", S({ site: "webgpu" }) === "webgpu");
  check("table: site auto, all pass -> webgpu", S({ site: "auto" }) === "webgpu");
  check("table: site AUTO (case) -> webgpu", S({ site: "AUTO" }) === "webgpu");
  check("table: site auto, probe fails -> vrend", S({ site: "auto", probe: { ok: false, reason: "x" } }) === "vrend");
  check("table: site webgpu, probe fails -> vrend", S({ site: "webgpu", probe: { ok: false, reason: "x" } }) === "vrend");
  check("table: site auto, probe not run -> vrend", S({ site: "auto", probe: null }) === "vrend");
  check("table: site auto, no navigator.gpu -> vrend", S({ site: "auto", hasGpu: false }) === "vrend");
  check("table: site webgpu, no navigator.gpu -> vrend", S({ site: "webgpu", hasGpu: false }) === "vrend");
  check("table: site auto, not Chrome/Edge desktop (D1) -> vrend", S({ site: "auto", browserOk: false }) === "vrend");
  check("table: site webgpu, other browser, probe ok -> webgpu", S({ site: "webgpu", browserOk: false }) === "webgpu");
  check("table: ?exec=vrend beats site auto", S({ override: "vrend", site: "auto" }) === "vrend");
  check("table: ?exec=webgpu beats site vrend", S({ override: "webgpu", site: "vrend" }) === "webgpu");
  check("table: ?exec=webgpu skips the probe and D1", S({ override: "webgpu", probe: { ok: false }, browserOk: false }) === "webgpu");
  check("table: ?exec=webgpu without navigator.gpu -> vrend", S({ override: "webgpu", hasGpu: false }) === "vrend");
  check("table: ?exec=proto -> proto", S({ override: "proto", site: "auto" }) === "proto");
  check("table: ?exec=nonsense falls to the site", S({ override: "nonsense", site: "webgpu" }) === "webgpu");
  check("table: ?exec=nonsense, no site -> vrend", S({ override: "nonsense" }) === "vrend");
  check("table: no WebGL2 -> none", S({ hasGL2: false, site: "auto" }) === "none");
  check("table: no WebGL2 beats ?exec=webgpu -> none", S({ hasGL2: false, override: "webgpu" }) === "none");

  // ---- 2. the probe and the browser rule
  const P = (a) => sel.checkAdapter(a).ok;
  check("adapter: good passes", P(goodAdapter()));
  check("adapter: none fails", !P(null));
  check("adapter: fallback (software) fails", !P(goodAdapter({}) && { ...goodAdapter(), isFallbackAdapter: true }));
  check("adapter: info.isFallbackAdapter fails", !P({ ...goodAdapter(), isFallbackAdapter: undefined, info: { isFallbackAdapter: true } }));
  check("adapter: 128 MiB storage binding passes", P(goodAdapter({ maxStorageBufferBindingSize: 128 << 20 })));
  check("adapter: 64 MiB storage binding fails", !P(goodAdapter({ maxStorageBufferBindingSize: 64 << 20 })));
  check("adapter: 2 bind groups fail", !P(goodAdapter({ maxBindGroups: 2 })));
  check("adapter: a missing limit fails", !P(goodAdapter({ maxColorAttachments: undefined })));
  check("adapter: reason names the limit", /maxBindGroups 2 < 4/.test(sel.checkAdapter(goodAdapter({ maxBindGroups: 2 })).reason));
  check("probe: no navigator.gpu", (await sel.probeAdapter(undefined)).ok === false);
  check("probe: adapter null", (await sel.probeAdapter({ requestAdapter: async () => null })).ok === false);
  check("probe: requestAdapter throws", (await sel.probeAdapter({ requestAdapter: async () => { throw new Error("boom"); } })).ok === false);
  const hung = await sel.probeAdapter({ requestAdapter: () => new Promise(() => {}) }, 30);
  check("probe: a hung requestAdapter times out", hung.ok === false && /timeout/.test(hung.reason), hung);
  check("probe: good adapter", (await sel.probeAdapter({ requestAdapter: async () => goodAdapter() })).ok === true);
  const B = (uad) => sel.browserAllowed({ userAgentData: uad });
  check("browser: Chrome desktop", B(chrome));
  check("browser: Edge desktop", B({ mobile: false, brands: [{ brand: "Microsoft Edge" }, { brand: "Chromium" }] }));
  check("browser: Chrome on a phone", !B({ ...chrome, mobile: true }));
  check("browser: Brave / Opera (not on D1's list)", !B({ mobile: false, brands: [{ brand: "Brave" }, { brand: "Chromium" }] }));
  check("browser: Firefox / Safari (no userAgentData)", !B(undefined));

  // ---- 3. the page flow
  const flow = async (opts, wantWorker, name, wantFetch) => {
    const p = page(opts);
    p.announce();
    await settle();
    const w = p.workers.map((x) => x.url).join(",");
    check("flow: " + name + " -> " + wantWorker, w === wantWorker, { workers: w, select: p.ctx.rvGpu.stats.select });
    if (wantFetch !== undefined) {
      check("flow: " + name + " fetched rv-site.json: " + wantFetch, (p.fetched.length > 0) === wantFetch, p.fetched);
    }
    return p;
  };
  await flow({ site: { gpu_exec: "vrend" }, userAgentData: chrome }, "gpu-exec.js", "site vrend, Chrome", true);
  await flow({ site: {}, userAgentData: chrome }, "gpu-exec.js", "site without gpu_exec");
  await flow({ site: undefined, userAgentData: chrome }, "gpu-exec.js", "rv-site.json unreachable");
  await flow({ site: { gpu_exec: "auto" }, siteStatus: 500, userAgentData: chrome }, "gpu-exec.js", "rv-site.json 500");
  await flow({ site: { gpu_exec: "auto" }, userAgentData: chrome }, "rvgpu-worker.js", "site auto, Chrome, good adapter");
  await flow({ site: { gpu_exec: "auto" }, userAgentData: undefined }, "gpu-exec.js", "site auto, Firefox/Safari (D1)");
  await flow({ site: { gpu_exec: "auto" }, hasGpu: false, userAgentData: chrome }, "gpu-exec.js", "site auto, no navigator.gpu");
  await flow({ site: { gpu_exec: "auto" }, adapter: goodAdapter({ maxBindGroups: 2 }), userAgentData: chrome }, "gpu-exec.js", "site auto, weak adapter");
  await flow({ site: { gpu_exec: "auto" }, adapter: null, userAgentData: chrome }, "gpu-exec.js", "site auto, no adapter");
  await flow({ site: { gpu_exec: "webgpu" }, userAgentData: undefined }, "rvgpu-worker.js", "site webgpu, other browser");
  await flow({ search: "?exec=webgpu", site: { gpu_exec: "vrend" } }, "rvgpu-worker.js", "?exec=webgpu over site vrend", false);
  await flow({ search: "?exec=vrend", site: { gpu_exec: "auto" }, userAgentData: chrome }, "gpu-exec.js", "?exec=vrend over site auto", false);
  await flow({ search: "?exec=auto", site: { gpu_exec: "vrend" }, userAgentData: chrome }, "rvgpu-worker.js", "?exec=auto over site vrend");
  await flow({ search: "?exec=webgpu", hasGpu: false }, "gpu-exec.js", "?exec=webgpu without navigator.gpu");

  // the WebGPU Worker finds no device: vrend takes the ring
  {
    const p = page({ site: { gpu_exec: "auto" }, userAgentData: chrome });
    p.announce();
    await settle();
    p.workers[0].onmessage({ data: { type: "ready", fallback: true, errors: ["no adapter"] } });
    check("chain: Worker without a device -> a vrend Worker follows",
          p.workers.map((x) => x.url).join(",") === "rvgpu-worker.js,gpu-exec.js" && p.workers[0].terminated);
  }
  // it dies before "ready"
  {
    const p = page({ site: { gpu_exec: "auto" }, userAgentData: chrome });
    p.announce();
    await settle();
    p.workers[0].onerror({ message: "wasm compile failed" });
    check("chain: Worker error before ready -> vrend", p.workers.map((x) => x.url).join(",") === "rvgpu-worker.js,gpu-exec.js"
          && p.ctx.rvGpu.stats.fallback.includes("compile"));
  }
  // it dies after "ready": no second start
  {
    const p = page({ site: { gpu_exec: "auto" }, userAgentData: chrome });
    p.announce();
    await settle();
    p.workers[0].onmessage({ data: { type: "ready", exec: "webgpu", renderer: "r", capsLen: 1 } });
    p.workers[0].onerror({ message: "late" });
    check("chain: Worker error after ready does not restart", p.workers.length === 1);
  }

  // the lazy start (VK-15) follows the executor the selection chose, not only ?exec=webgpu
  {
    const p = page({ site: { gpu_exec: "auto" }, userAgentData: chrome });
    p.announce({ lazy: true });
    await settle();
    const w32 = new Int32Array(p.memory.buffer);
    check("lazy: site auto/webgpu waits for the first program (exec_state 4, no Worker)",
          w32[(4096 + 76) >> 2] === 4 && p.workers.length === 0);
    p.announce({ lazy: true, want: true });
    await settle();
    check("lazy: the first program's connection starts it", p.workers.map((x) => x.url).join(",") === "rvgpu-worker.js");
    const q = page({ site: { gpu_exec: "vrend" }, userAgentData: chrome });
    q.announce({ lazy: true });
    await settle();
    check("lazy: vrend is not deferred", q.workers.map((x) => x.url).join(",") === "gpu-exec.js");
  }

  // ---- 4. a lost device
  {
    const p = page({ site: { gpu_exec: "auto" }, userAgentData: chrome });
    p.announce();
    await settle();
    const w32 = new Int32Array(p.memory.buffer);
    const H_EXEC_STATE = 76;
    w32[(4096 + H_EXEC_STATE) >> 2] = 1;
    p.workers[0].onmessage({ data: { type: "ready", exec: "webgpu", renderer: "r", capsLen: 1 } });
    p.workers[0].onmessage({ data: { type: "log", text: "WebGPU device lost: destroyed forced by the test" } });
    check("lost: exec_state is 2 in the ring", w32[(4096 + H_EXEC_STATE) >> 2] === 2, w32[(4096 + H_EXEC_STATE) >> 2]);
    check("lost: the Worker is stopped", p.workers[0].terminated);
    check("lost: one notice that says what happens to open programs",
          p.notices.length === 1 && /software/.test(p.notices[0]) && /reload/.test(p.notices[0]), p.notices);
    p.workers[0].onmessage({ data: { type: "device-lost", reason: "again", message: "" } });
    check("lost: reported once", p.notices.length === 1);
  }
  {
    // (vrend losing a context is not this: no ring write, no WebGPU notice)
    const p = page({ site: { gpu_exec: "vrend" }, userAgentData: chrome });
    p.announce();
    await settle();
    p.workers[0].onmessage({ data: { type: "log", text: "WebGPU device lost: x" } });
    check("lost: ignored under vrend", p.notices.length === 0 && !p.workers[0].terminated);
  }

  console.log(failed ? "FAIL " + failed + " of " + count : "PASS " + count + " checks");
  process.exit(failed ? 1 : 0);
})().catch((e) => {
  console.log("FAIL", e);
  process.exit(1);
});
