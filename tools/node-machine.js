#!/usr/bin/env node
// The Linux/Wasm machine without a browser: the same kernel, runtime
// (linux.js, linux-worker.js) and disks, with Node.js as the host. A task is
// a worker thread instead of a Web Worker. For work on what runs on the
// machine that does not need the browser itself: it starts in a second, it
// is a process of your own (no browser to share, nothing staged for others),
// and it is gone when the command is done.
//
//   node --experimental-wasm-memory64 tools/node-machine.js [options] 'COMMAND'
//
//   --runtime DIR      linux.js and linux-worker.js (as tools/patch-runtime.py leaves them)
//   --kernel FILE      vmlinux-wasm64_nommu.wasm
//   --initramfs FILE   (several: they are put one after the other, a later one's files win)
//   --disk NAME        the disk whose pieces the dev server has in disk/NAME/ (as dev.html?disk=NAME);
//                      what the machine writes to it is kept in memory and forgotten
//   --net              connect to the network relay (tools/net-relay.py)
//   --relay URL        where it is (default: the server's host, port 8001; Docker's: http://localhost:8081)
//   --server URL       the dev server (default http://localhost:8000)
//   --timeout MS       give up after this long (default 60000)
//   --keys JSON        [["text to wait for", "keys to type then"], ...]: a conversation with the program
//   --hook FILE.js     a module whose export is called, once the command has been typed, with the machine
//                      (os: its memory, its disks' words) and the time the command may take; it may look at
//                      the machine's memory while the command runs (tools/wl-dump.js: the window server's
//                      surface table, as PNG files), and is told when the command is done
//   --append WORDS     more for the kernel's command line (e.g. wasm_pool=1; wasm_cache_max=0: no cache cap)
//   --query STRING     what the runtime takes from a page's address (e.g. "?switches=main")
//   --busy-main MS     keep the main thread busy MS ms out of every 16, as a page that draws is (once booted)
//   --verbose          everything the console and the runtime say, as it comes, on standard error
//
// Prints what the command printed, exits with its status. COMMAND is typed
// into the machine's shell, as tools/bench-browser.js sh does.
//
// It is not the browser: the engine is the V8 of this Node.js (older than a
// current Chrome's), so nothing measured here says how fast something is
// there, and what depends on the browser (the canvas, OPFS) is not here.
// Node 22 has the 64-bit memory of WebAssembly behind a flag and with the
// older form of its JavaScript interface, which is translated below.
"use strict";

const { Worker: NodeWorker } = require("worker_threads");
const fs = require("fs");
const path = require("path");

const args = process.argv.slice(2);
const options = { initramfs: [], timeout: 60000, server: "http://localhost:8000", keys: "[]" };
let command = null;
for (let i = 0; i < args.length; i++) {
  const arg = args[i];
  if (arg === "--verbose" || arg === "--net") {
    options[arg.slice(2)] = true;
  } else if (arg === "--initramfs") {
    options.initramfs.push(args[++i]);
  } else if (arg.startsWith("--")) {
    options[arg.slice(2)] = args[++i];
  } else {
    command = arg;
  }
}
if (!command || !options.runtime || !options.kernel || !options.initramfs.length) {
  console.error("usage: node --experimental-wasm-memory64 tools/node-machine.js --runtime DIR --kernel FILE --initramfs FILE [options] 'COMMAND'");
  process.exit(2);
}

// --- Web Workers as worker threads. What runs in one is linux-worker.js or disk-worker.js, by its URL.
const workerSource = fs.readFileSync(path.join(options.runtime, "linux-worker.js"), "utf8");
const diskSource = fs.readFileSync(path.join(__dirname, "www", "disk-worker.js"), "utf8");
const taskShim = `
const { parentPort, workerData } = require("worker_threads");
globalThis.self = globalThis;
self.postMessage = (m) => parentPort.postMessage(m);
// Node's memory for WebAssembly counts in numbers where the runtime counts in BigInts.
const patch = (memory) => {
  const grow = memory.grow;
  memory.grow = function (n) { return BigInt(grow.call(this, Number(n))); };
};
parentPort.on("message", (d) => { if (d && d.memory) patch(d.memory); self.onmessage({ data: d }); });
// What a browser tells a worker by events, Node tells its process: passed on in the browser's form.
{
  const realProcess = process;
  const listeners = { unhandledrejection: [], rejectionhandled: [] };
  globalThis.addEventListener = (type, fn) => { if (listeners[type]) listeners[type].push(fn); };
  realProcess.on("unhandledRejection", (reason, promise) => {
    for (const f of listeners.unhandledrejection) f({ promise, reason, preventDefault() {} });
  });
  realProcess.on("rejectionHandled", (promise) => { for (const f of listeners.rejectionhandled) f({ promise }); });
}
// What this Node.js keeps on the global object for itself, and a program on the machine must not find there
// (a Node.js program would take this one's connections for its own).
// (Its HTTP client, undici, registers itself there when it is first loaded, which a message between
// threads brings about sooner or later: so load it now, and then take the registration away.)
void globalThis.Headers;
// Node.js imports no modules from blobs: hostnode is told to make its modules another way (binding/esm.js).
globalThis.__hostnodeModuleURLs = "data";
globalThis[Symbol.for("undici.globalDispatcher.1")] = undefined;
(0, eval)(workerData.source + "\\n//# sourceURL=linux-worker.js");
`;
const diskShim = `
const { parentPort, workerData } = require("worker_threads");
const { execFileSync } = require("child_process");
globalThis.postMessage = (m) => parentPort.postMessage(m);
globalThis.onmessage = null;
parentPort.on("message", (d) => globalThis.onmessage({ data: d }));
// A piece of the disk, fetched while the task that needs it waits.
globalThis.XMLHttpRequest = class {
  open(method, url) { this.url = url; }
  send() {
    try {
      const out = execFileSync("curl", ["-s", "--fail", this.url], { maxBuffer: 64 << 20 });
      this.status = 200;
      this.response = out.buffer.slice(out.byteOffset, out.byteOffset + out.byteLength);
    } catch (error) {
      this.status = 404;
    }
  }
};
// No private file system here: the worker then keeps what is written in memory.
Object.defineProperty(globalThis, "navigator", { configurable: true, value: { storage: { getDirectory: async () => {
  throw Object.assign(new Error("not under Node"), { name: "NotSupportedError" });
} } } });
(0, eval)(workerData.source + "\\n//# sourceURL=disk-worker.js");
`;
class WebWorker {
  constructor(url, workerOptions) {
    const disk = String(url).includes("disk-worker");
    this.w = new NodeWorker(disk ? diskShim : taskShim, {
      eval: true,
      workerData: { source: disk ? diskSource : workerSource },
      name: workerOptions && workerOptions.name,
      // NM_WORKER_HEAP_MB: another limit for every task's JavaScript heap (maxOldGenerationSizeMb). Tried against
      // the ERR_WORKER_OUT_OF_MEMORY seen in runs with plasmashell's panel (2026-10-10); 4096 and 8192 did not
      // make it go away.
      ...(process.env.NM_WORKER_HEAP_MB ? { resourceLimits: { maxOldGenerationSizeMb: Number(process.env.NM_WORKER_HEAP_MB) } } : {}),
    });
    this.w.on("message", (d) => this.onmessage && this.onmessage({ data: d }));
    this.w.on("error", (error) => {
      if (this.onerror) {
        try {
          this.onerror(error);
          return;
        } catch (e) {
        }
      }
      console.error("[worker " + (workerOptions && workerOptions.name) + (this.runner ? " " + this.runner : "") + "]", error);
    });
  }
  postMessage(m) {
    if (m && m.method === "assign" && m.runner_name) {
      this.runner = m.runner_name;	// (whose task a spare worker became: said with its errors)
    }
    this.w.postMessage(m);
  }
  terminate() {
    this.w.terminate();
  }
}
globalThis.Worker = WebWorker;
const RealMemory = WebAssembly.Memory;
WebAssembly.Memory = function (d) {
  return new RealMemory({ initial: Number(d.initial), maximum: Number(d.maximum), shared: d.shared, index: d.address });
};
globalThis.confirm = () => false;
// The relay lets in pages of the dev server; this stands in for one.
const RealWebSocket = globalThis.WebSocket;
globalThis.WebSocket = function (url) {
  return new RealWebSocket(url, { headers: { Origin: options.server } });
};
globalThis.WebSocket.OPEN = RealWebSocket.OPEN;
globalThis.requestAnimationFrame = () => {};

const linux = (0, eval)(fs.readFileSync(path.join(options.runtime, "linux.js"), "utf8") + "\n;linux");

(async () => {
  const vmlinux = await WebAssembly.compile(fs.readFileSync(options.kernel));
  const initrd = Buffer.concat(options.initramfs.map((file) => fs.readFileSync(file)));
  let output = "";
  let ready = null;
  const log = (text) => {
    // (What goes wrong in a task is said here and nowhere else: always shown.)
    if (options.verbose || /crash|panic|error/i.test(text)) {
      process.stderr.write("[log] " + text + "\n");
    }
  };
  const console_write = (data) => {
    output += data;
    if (options.verbose) {
      process.stderr.write(data);
    }
    if (ready && output.endsWith("# ")) {
      const r = ready;
      ready = null;
      r();
    }
  };
  let cmdline = "maxcpus=3 nohz_full=0,2-63 rcu_nocbs=0,2-63 root=/dev/ram0 rootfstype=ramfs init=/init console=hvc console=ttyS0"
    + " epoch=" + Math.floor(Date.now() / 1000) + " term=120x30 wasm_pool=2 wasm_cache_max=256";
  // --append "WORDS": more of the kernel's command line (a later wasm_pool= or wasm_cache_max= wins over the one above).
  if (options.append) {
    cmdline += " " + options.append;
  }
  let netUrl = null;
  if (options.net) {
    const relay = options.relay || options.server.replace(/:\d+$/, ":8001");
    let address = 2;
    try {
      address = Number(await (await fetch(relay + "/address", { headers: { Origin: options.server } })).text()) || 2;
    } catch (error) {
    }
    netUrl = relay.replace(/^http/, "ws") + "/?ip=" + address;
    cmdline += " ip=10.0.5." + address;
  }
  const disks = !options.disk ? null : [{
    worker: "disk-worker.js", url: options.server + "/disk/" + options.disk + "/", name: options.disk, reset: false,
  }];
  const began = Date.now();
  const booted = new Promise((resolve) => {
    ready = resolve;
  });
  const timer = setTimeout(() => {
    process.stderr.write("TIMEOUT\n" + output.slice(-4000) + "\n");
    process.exit(124);
  }, Number(options.timeout));
  globalThis.rv_query = options.query || "";
  const os = await linux("linux-worker.js", "wasm64_nommu", vmlinux, cmdline,
                         initrd.buffer.slice(initrd.byteOffset, initrd.byteOffset + initrd.byteLength),
                         log, console_write, netUrl, null, disks);
  await booted;
  process.stderr.write("booted in " + (Date.now() - began) + " ms\n");
  if (options["busy-main"]) {
    const busy = Number(options["busy-main"]);
    setInterval(() => {
      const until = performance.now() + busy;
      while (performance.now() < until) {
      }
    }, 16);
  }
  const marker = "__rc" + Math.random().toString(36).slice(2) + "_";
  const start = output.length;
  os.key_input(command + "; echo " + marker + "$?\r");
  const pattern = new RegExp(marker + "(\\d+)");
  const steps = JSON.parse(options.keys);
  let seenUpTo = start;
  let hook = null;
  if (options.hook) {
    hook = { done: () => {} };
    const result = require(path.resolve(options.hook))(os, { timeout: Number(options.timeout), output: () => output.slice(start) });
    if (result && typeof result.done === "function") {
      hook = result;
    }
  }
  const poll = setInterval(() => {
    while (steps.length) {
      const at = output.indexOf(steps[0][0], seenUpTo);
      if (at < 0) {
        break;
      }
      seenUpTo = at + steps[0][0].length;
      os.key_input(steps.shift()[1]);
    }
    const text = output.slice(start);
    const m = text.match(pattern);
    if (!m) {
      return;
    }
    clearInterval(poll);
    clearTimeout(timer);
    // (Without the echo of what was typed: everything up to the first line break.)
    let out = text.slice(0, m.index);
    out = out.slice(out.indexOf("\n") + 1).replace(/\r/g, "");
    process.stdout.write(out);
    Promise.resolve(hook && hook.done()).then(() => process.exit(Number(m[1])));
  }, 20);
})().catch((error) => {
  console.error(error);
  process.exit(125);
});
