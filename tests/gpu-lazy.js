// VK-15: the page's WebGPU executor starts on the first GPU program, not at boot (tools/www/gpu-exec.js, gpuserver's
// `lazy`/`want` announcement fields, exec_state 4). Runs gpu-exec.js in a vm with a stand-in page: counts the Workers it
// spawns (rvgpu-worker.js is the only thing that fetches/compiles rvgpu.wasm, once per spawn). Node 18+, no browser.
//   node tests/gpu-lazy.js
"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm"), assert = require("assert");
const src = fs.readFileSync(path.join(__dirname, "../tools/www/gpu-exec.js"), "utf8");

function page(query) {
  const spawned = [], logs = [], channels = {};
  const memory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
  class Worker {
    constructor(url) { this.url = url; this.msgs = []; spawned.push(this); }
    postMessage(m) { this.msgs.push(m); }
    terminate() {}
  }
  class Chan { constructor() { this.onmessage = null; this.sent = []; } postMessage(m) { this.sent.push(m); } }
  const gl = { getExtension: () => null, getParameter: () => "stub" };
  const win = {
    linuxOs: { memory }, rvDisplay: { gl },
    rvChannel: (name) => channels[name] || (channels[name] = new Chan()),
  };
  const ctx = {
    window: win, location: { search: query }, document: { currentScript: { src: "https://x.test/www/gpu-exec.js" }, hidden: false },
    navigator: { gpu: {} }, Worker, console: { log: (...a) => logs.push(a.join(" ")) },
    performance, Atomics, Int32Array, URLSearchParams, setInterval: () => 0, clearInterval() {}, WebAssembly,
  };
  vm.createContext(ctx);
  vm.runInContext(src, ctx);
  const state = (ring) => Atomics.load(new Int32Array(memory.buffer), (ring + 76) >> 2);
  // (VK-13: the page chooses its executor asynchronously, rv-site.json and the adapter probe, so an announcement takes
  // effect a turn later: announce() waits for that.)
  const announce = async (extra) => {
    channels["rv-gpu"].onmessage({ data: Object.assign({ type: "gpu", version: 2, ring: 4096, pid: 7, caps: 8192, capsSize: 4096 }, extra) });
    await new Promise((resolve) => setImmediate(resolve));
  };
  return { spawned, logs, announce, state, win, ctx };
}

let n = 0;
(async () => {
const check = async (name, fn) => { await fn(); n++; console.log("ok   " + name); };

await check("lazy: five announcements, no program yet: no Worker, exec_state 4", async () => {
  const p = page("?exec=webgpu");
  for (let i = 0; i < 5; i++) await p.announce({ lazy: 1, want: false });
  assert.strictEqual(p.spawned.length, 0);
  assert.strictEqual(p.state(4096), 4);
});
await check("lazy: the first program (want) starts one Worker, rvgpu-worker.js; more announcements start no second", async () => {
  const p = page("?exec=webgpu");
  await p.announce({ lazy: 1, want: false });
  await p.announce({ lazy: 1, want: true });
  await p.announce({ lazy: 1, want: true });
  await p.announce({ lazy: 1, want: false });
  assert.strictEqual(p.spawned.length, 1);
  assert.ok(p.spawned[0].url.endsWith("/www/rvgpu-worker.js"));
  assert.strictEqual(p.spawned[0].msgs.filter((m) => m.type === "start").length, 1);
});
await check("an older gpuserver (no `lazy`): starts at once, as before", async () => {
  const p = page("?exec=webgpu");
  await p.announce({});
  assert.strictEqual(p.spawned.length, 1);
});
await check("?rvgpu=eager: starts at once", async () => {
  const p = page("?exec=webgpu&rvgpu=eager");
  await p.announce({ lazy: 1, want: false });
  assert.strictEqual(p.spawned.length, 1);
});
await check("no navigator.gpu: vrend at once (nothing deferred)", async () => {
  const p = page("?exec=webgpu");
  p.ctx.navigator.gpu = undefined;
  await p.announce({ lazy: 1, want: false });
  assert.strictEqual(p.spawned.length, 1);
  assert.ok(p.spawned[0].url.endsWith("gpu-exec.js"));
});
await check("default executor (vrend) is not deferred", async () => {
  const p = page("");
  await p.announce({ lazy: 1, want: false });
  assert.strictEqual(p.spawned.length, 1);
});
// VK-14: the video decoder's port (rvGpu.vdecPort) reaches the WebGPU executor's Worker, also when that starts later
await check("vdecPort: a started WebGPU Worker gets the port at once (true)", async () => {
  const p = page("?exec=webgpu&rvgpu=eager");
  await p.announce({});
  const port = { id: 1 };
  assert.strictEqual(p.win.rvGpu.vdecPort(port), true);
  assert.strictEqual(p.spawned[0].msgs.filter((m) => m.type === "vdec-port" && m.port === port).length, 1);
});
await check("vdecPort: while the lazy executor waits the port is kept, and goes to the Worker that the first program starts", async () => {
  const p = page("?exec=webgpu");
  await p.announce({ lazy: 1, want: false });
  const port = { id: 2 };
  assert.strictEqual(p.win.rvGpu.vdecPort(port), true);
  assert.strictEqual(p.win.rvGpu.vdecPort({ id: 3 }), false);	// (one decoder: a second port has nowhere to go)
  assert.strictEqual(p.spawned.length, 0);
  await p.announce({ lazy: 1, want: true });
  assert.strictEqual(p.spawned.length, 1);
  assert.strictEqual(p.spawned[0].msgs.filter((m) => m.type === "vdec-port" && m.port === port).length, 1);
});
await check("vdecPort: no executor at all (?gpu=off style, vrend not started): false", async () => {
  const p = page("");
  assert.strictEqual(p.win.rvGpu.vdecPort({ id: 4 }), false);
});
await check("vdecPort: the WebGPU Worker finds no device and hands the port back: the vrend Worker gets it", async () => {
  const p = page("?exec=webgpu&rvgpu=eager");
  await p.announce({});
  const port = { id: 5 };
  p.win.rvGpu.vdecPort(port);
  const handed = { id: 6 };
  p.spawned[0].onmessage({ data: { type: "ready", fallback: true, errors: ["no adapter"], vdecPort: handed } });
  assert.strictEqual(p.spawned.length, 2);
  assert.ok(p.spawned[1].url.endsWith("gpu-exec.js"));
  assert.strictEqual(p.spawned[1].msgs.filter((m) => m.type === "vdec-port" && m.port === handed).length, 1);
});
console.log(n + " checks, 0 failed");
})();
