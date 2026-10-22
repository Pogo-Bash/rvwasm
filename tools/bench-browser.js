#!/usr/bin/env node
// Run tests/bench.sh in a browser, under rv on Linux/Wasm or under qemu-wasm,
// and print the timings. Each run uses a freshly started Chrome with an empty
// profile, driven over the DevTools protocol, with the throttling of
// background pages switched off: a page that is not on screen is otherwise
// slowed down about three times, which ruins a measurement.
//
//   node tools/bench-browser.js sh 'COMMAND' [TIMEOUT_MS] [--image NAME] [--disk NAME] [--then 'COMMAND'] [--query K=V]
//                                             [--profile FILE --profile-at MS --profile-for MS]
//                                                (run one command on Linux/Wasm, print the output;
//                                                --then: load the page again and run another, to see what a
//                                                disk has kept; --profile: sample every Worker of the machine
//                                                for MS ms, starting MS ms into the command, with the
//                                                browser's own profiler, and write where the time went, by
//                                                function and summed over the Workers, to FILE (JSON) and the
//                                                top of it to stderr. The emulator's functions have their
//                                                names; a region of translated code is "r".)
//   node tools/bench-browser.js rv|qemu [RUNS] [--headed] [--url BASE] [--dir NAME] [--count-jit]
//
// --count-jit (qemu): count the wasm modules qemu-wasm compiles while running,
// one per hot translated block, to show that its JIT is at work and not only
// its interpreter. The count is taken inside every worker thread.
//
// Needs the dev server (default http://localhost:8000), Node 22 or newer and
// Chrome for Testing in tools/chrome (or CHROME=path of another Chrome).
// Prints one line of JSON per run, then the median, minimum and maximum.

const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

// One test browser at a time on this computer: tools/with-browser.sh sees to that, and says so by RV_BROWSER.
if (process.env.RV_BROWSER != "1") {
  console.error("run this under tools/with-browser.sh (one test browser at a time): tools/with-browser.sh WHO node " + process.argv.slice(1).join(" "));
  process.exit(2);
}

const args = process.argv.slice(2);
const system = args[0];
const runs = parseInt(args[1], 10) || 3;
const headed = args.includes("--headed");
const countJit = args.includes("--count-jit");
const base = args.includes("--url") ? args[args.indexOf("--url") + 1] : "http://localhost:8000";
// Chrome for Testing, a separate application from the user's own Chrome
// (tools/chrome, unpacked from the chrome-for-testing mac-arm64 download).
const CHROME = process.env.CHROME || path.join(__dirname,
  "chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");

const RV_BENCH = "mkdir -p /t; cd /t; PATH=/rvbin COREMARK=/rv/coremark BUSYBOX=/bin/busybox-rv64 /rvbin/sh /rv/bench.sh";
// --dir NAME: the qemu-wasm build to use, e.g. qemu-nodebug (default qemu).
const qemuDir = args.includes("--dir") ? args[args.indexOf("--dir") + 1] : "qemu";
const QEMU_URL = base + "/" + qemuDir + "/?append=bench&cpu=" + encodeURIComponent("rv64,zba=off,zbb=off,zbc=off,zbs=off");

if (system === "sh") {
  // bench-browser.js sh 'COMMAND': run a command on the Linux/Wasm system, print its output.
} else if (system !== "rv" && system !== "qemu") {
  console.error("usage: bench-browser.js rv|qemu [RUNS] [--headed] [--url BASE] [--dir NAME] [--count-jit]");
  process.exit(2);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Start Chrome; resolve with the process and the browser's DevTools address.
function startChrome(profile) {
  const chrome = spawn(CHROME, [
    ...(headed ? [] : ["--headless=new"]),
    "--user-data-dir=" + profile,
    "--remote-debugging-port=0",
    "--no-first-run", "--no-default-browser-check",
    "--window-size=1100,1500",	// room for the machine's display (1024x768) and the console below it
    "--disable-background-timer-throttling",
    "--disable-renderer-backgrounding",
    "--disable-backgrounding-occluded-windows",
    "about:blank",
  ], { stdio: ["ignore", "ignore", "pipe"] });
  return new Promise((resolve, reject) => {
    let err = "";
    chrome.stderr.on("data", (data) => {
      err += data;
      const m = err.match(/DevTools listening on (ws:\/\/\S+)/);
      if (m) {
        resolve({ chrome, address: m[1] });
      }
    });
    chrome.on("exit", () => reject(new Error("Chrome exited: " + err)));
  });
}

// A DevTools connection: send(method, params, sessionId) returns the result;
// onEvent, if set, is called with every event.
function connect(address) {
  const ws = new WebSocket(address);
  const waiting = new Map();
  const connection = { onEvent: null };
  let next = 1;
  ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    // A dialog stops the page's main thread, and with it the machine, until someone answers: say what it said, and answer.
    if (msg.method == "Page.javascriptDialogOpening") {
      console.error("the page asked (" + msg.params.type + "): " + msg.params.message.replace(/\n+/g, " | "));
      ws.send(JSON.stringify({ id: next++, method: "Page.handleJavaScriptDialog", params: { accept: true },
                               sessionId: msg.sessionId }));
    }
    if (!msg.id && connection.onEvent) {
      connection.onEvent(msg);
    }
    if (msg.id && waiting.has(msg.id)) {
      const { resolve, reject } = waiting.get(msg.id);
      waiting.delete(msg.id);
      if (msg.error) {
        reject(new Error(msg.error.message));
      } else {
        resolve(msg.result);
      }
    }
  };
  // The connection does not survive the computer going to sleep, and nothing sent on a dead one is ever answered:
  // whoever waits for an answer is told, instead of waiting for good with the browser's lock held.
  ws.onclose = () => {
    for (const { reject } of waiting.values()) {
      reject(new Error("the connection to the browser is gone (did the computer sleep?)"));
    }
    waiting.clear();
  };
  return new Promise((resolve, reject) => {
    ws.onerror = () => reject(new Error("cannot connect to Chrome"));
    ws.onopen = () => resolve(Object.assign(connection, {
      send(method, params = {}, sessionId) {
        const id = next++;
        ws.send(JSON.stringify({ id, method, params, sessionId }));
        return new Promise((resolve, reject) => waiting.set(id, { resolve, reject }));
      },
      close: () => ws.close(),
    }));
  });
}

// Run in each worker before its own code: count the modules compiled there
// and report the count on the console now and then. (A worker cannot be asked
// afterwards: it may be blocked in a wait. The workers receive QEMU itself
// already compiled, so that is not counted.)
const COUNT_MODULES = `(() => { let modules = 0, bytes = 0;
  WebAssembly.Module = new Proxy(WebAssembly.Module, { construct(target, args) {
    modules++; bytes += args[0].byteLength || 0;
    if (modules < 10 || modules % 50 === 0) { console.debug("__jit " + modules + " " + bytes); }
    return new target(...args); } }); })()`;

// "BENCH name ms check" lines of some output, as { name: [ms, check] }.
function parseBench(text) {
  const result = {};
  for (const line of text.split("\n")) {
    const m = line.match(/^BENCH (\S+) (\d+) (\S+)/);
    if (m) {
      result[m[1]] = [parseInt(m[2], 10), m[3]];
    }
  }
  return result;
}

async function oneRun() {
  // PROFILE_DIR=DIR: that profile, kept afterwards (a second run with it is the next visit of the same browser:
  // what the page kept in its private file system, and the browser's caches, are there). Else a new one, removed.
  const profile = process.env.PROFILE_DIR || fs.mkdtempSync(path.join(os.tmpdir(), "rvbench-"));
  fs.mkdirSync(profile, { recursive: true });
  const { chrome, address } = await startChrome(profile);
  try {
    const cdp = await connect(address);
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const evaluate = async (expression) => {
      const r = await cdp.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId);
      if (r.exceptionDetails) {
        throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      }
      return r.result.value;
    };
    await cdp.send("Page.enable", {}, sessionId);
    const started = Date.now();
    let text, extra = {};
    if (system === "sh") {
      // --image NAME: boot initramfs-NAME-<variant>.cpio.gz (e.g. debian) instead of the usual image.
      // --disk NAME: give the machine the disk whose pieces are in disk/NAME/ (tools/split-disk.py).
      const query = [];
      if (args.includes("--image")) {
        query.push("image=" + args[args.indexOf("--image") + 1]);
      }
      if (args.includes("--disk")) {
        query.push("disk=" + args[args.indexOf("--disk") + 1]);
      }
      // --pool N: how many CPUs the emulator's tasks share (0: none, every task gets its own).
      if (args.includes("--pool")) {
        query.push("pool=" + args[args.indexOf("--pool") + 1]);
      }
      // --query K=V (as often as wanted): more of the page's parameters, e.g. jitcache=0.
      args.forEach((arg, i) => {
        if (arg == "--query") {
          query.push(args[i + 1]);
        }
      });
      const url = base + "/dev.html" + (query.length ? "?" + query.join("&") : "");
      const timeout = parseInt(args[2], 10) || 120000;
      // A page that dies (out of memory, most likely) answers nothing any more: say so instead of waiting.
      // --page-log RE: the page's console lines that match RE (e.g. "rvgpu|vkr|WebGPU"), to stderr.
      const pageLog = args.includes("--page-log") ? new RegExp(args[args.indexOf("--page-log") + 1]) : null;
      cdp.onEvent = (msg) => {
        if (msg.method == "Inspector.targetCrashed" || msg.method == "Target.targetCrashed") {
          console.error("the page crashed");
          chrome.kill();
          process.exit(3);
        }
        if (pageLog && msg.method == "Runtime.consoleAPICalled") {
          const text = msg.params.args.map((a) => a.value !== undefined ? String(a.value) : a.description || "").join(" ");
          if (pageLog.test(text)) {
            console.error("[page] " + text);
          }
        }
      };
      await cdp.send("Inspector.enable", {}, sessionId).catch(() => {});
      if (pageLog) {
        await cdp.send("Runtime.enable", {}, sessionId).catch(() => {});
      }
      // --profile: every Worker the page starts is attached to as it starts, with the profiler ready in it.
      const profiling = args.includes("--profile");
      const profileFile = profiling ? args[args.indexOf("--profile") + 1] : null;
      const profileAt = args.includes("--profile-at") ? parseInt(args[args.indexOf("--profile-at") + 1], 10) : 5000;
      const profileFor = args.includes("--profile-for") ? parseInt(args[args.indexOf("--profile-for") + 1], 10) : 10000;
      const workers = new Set();
      if (profiling) {
        const attach = { autoAttach: true, waitForDebuggerOnStart: true, flatten: true };
        const previous = cdp.onEvent;
        cdp.onEvent = (msg) => {
          if (previous) {
            previous(msg);
          }
          if (msg.method === "Target.attachedToTarget") {
            const worker = msg.params.sessionId;
            workers.add(worker);
            cdp.send("Profiler.enable", {}, worker).catch(() => {});
            cdp.send("Profiler.setSamplingInterval", { interval: 1000 }, worker).catch(() => {});
            cdp.send("Target.setAutoAttach", attach, worker).catch(() => {});
            cdp.send("Runtime.runIfWaitingForDebugger", {}, worker).catch(() => {});
          } else if (msg.method === "Target.detachedFromTarget") {
            workers.delete(msg.params.sessionId);
          }
        };
        await cdp.send("Target.setAutoAttach", attach, sessionId);
      }
      // Sample every Worker there is for `profileFor` ms; the self time of every function, summed over the Workers.
      const profileWorkers = async () => {
        const started = [...workers];
        await Promise.all(started.map((w) => cdp.send("Profiler.start", {}, w).catch(() => {})));
        await sleep(profileFor);
        const self = new Map();
        let total = 0, sampled = 0;
        for (const w of started) {
          const r = await cdp.send("Profiler.stop", {}, w).catch(() => null);
          if (!r) {
            continue;
          }
          sampled++;
          const byId = new Map(r.profile.nodes.map((n) => [n.id, n]));
          const counts = new Map();
          for (const id of r.profile.samples) {
            counts.set(id, (counts.get(id) || 0) + 1);
          }
          for (const [id, count] of counts) {
            const f = byId.get(id).callFrame;
            const url = (f.url || "").replace(/^.*\//, "");
            const name = (f.functionName || "(anonymous)") + (url && !url.startsWith("wasm") ? " " + url : "")
                         + (url.startsWith("wasm") && f.functionName !== "r" ? " [rv]" : "");
            self.set(name, (self.get(name) || 0) + count);
            total += count;
          }
        }
        const sorted = [...self.entries()].sort((a, b) => b[1] - a[1]);
        console.error(`[profile: ${sampled} of ${started.length} Workers sampled for ${profileFor} ms from ${profileAt} ms in, `
                      + `${total} samples]`);
        for (const [name, count] of sorted.slice(0, 40)) {
          console.error(`  ${String((100 * count / total).toFixed(1)).padStart(5)}%  ${name}`);
        }
        fs.writeFileSync(profileFile, JSON.stringify({ workers: started.length, sampled, total,
          self: Object.fromEntries(sorted) }, null, 1));
      };
      const run = async (command) => {
        await cdp.send("Page.navigate", { url: url }, sessionId);
        // (The page's script may not have run yet, on a busy computer.)
        for (let i = 0; i < 100 && !await evaluate("typeof linuxReady != 'undefined'").catch(() => false); i++) {
          await sleep(200);
        }
        // "A ;; B ;; C" is three commands, run one after the other and reported as they end; the first
        // that does not end in time stops the run (and the page's log says what the machine was doing).
        const parts = command.split(" ;; ");
        let r = null;
        // (The profile window is counted from the start of the first command.)
        const profiled = profiling ? sleep(profileAt).then(profileWorkers) : null;
        for (const part of parts) {
          const one = await evaluate(`(async () => { await linuxReady;
            return await linuxRun(${JSON.stringify(part)}, ${timeout}); })()`);
          if (parts.length > 1) {
            const cpus = await evaluate("linuxLog.filter((line) => line.startsWith('Starting cpu')).length");
            console.error(`[${one.rc === null ? "NOT DONE" : "rc " + one.rc}, ${Math.round(one.ms)} ms, `
                          + `${cpus} more cpus] ${part}`);
          }
          r = r ? { rc: one.rc, ms: r.ms + one.ms, out: r.out + one.out } : one;
          if (one.rc === null) {
            r.out += "\n---- the page's log ----\n" + await evaluate("linuxLog.slice(-12).join('\\n')") + "\n";
            break;
          }
        }
        // (The store of translated code kept now, not a minute later: a run after --then is to find it.)
        await evaluate("typeof jitcache != 'undefined' && jitcache.save_now ? jitcache.save_now().then(() => true) : false")
          .catch(() => {});
        r.log = await evaluate("linuxLog.filter((line) => line.startsWith('[Disk') || line.startsWith('[JIT')).join('\\n')");
        if (profiled) {
          await profiled;
        }
        return r;
      };
      let r = await run(args[1]);
      if (args.includes("--then")) {
        const first = r;
        r = await run(args[args.indexOf("--then") + 1]);
        r.out = first.out + "---- loaded again ----\n" + r.out;
        r.log = first.log + "\n---- loaded again ----\n" + r.log;
      }
      // EVAL=expression: something to evaluate in the page afterwards (printed as JSON).
      if (process.env.EVAL) {
        r.evaluated = await evaluate(process.env.EVAL);
      }
      // SHOT=file.png: a picture of the page afterwards (the machine's display is the canvas at its top).
      if (process.env.SHOT) {
        await sleep(300);
        const shot = await cdp.send("Page.captureScreenshot", { format: "png" }, sessionId);
        fs.writeFileSync(process.env.SHOT, Buffer.from(shot.data, "base64"));
      }
      cdp.close();
      return r;
    } else if (system === "rv") {
      await cdp.send("Page.navigate", { url: base + "/dev.html" }, sessionId);
      await sleep(500);
      // The native wasm CoreMark first: a check that the browser runs at full speed.
      const native = await evaluate(`(async () => { await linuxReady;
        return (await linuxRun('coremark-wasm 0 0 0x66 2000', 60000)).out; })()`);
      extra.native_coremark_ticks = parseInt((native.match(/Total ticks *: *(\d+)/) || [])[1], 10);
      extra.boot_ms = Date.now() - started;
      text = await evaluate(`(async () => (await linuxRun(${JSON.stringify(RV_BENCH)}, 1200000)).out)()`);
    } else {
      const counts = new Map();   // worker session -> [modules, bytes]
      if (countJit) {
        const attach = { autoAttach: true, waitForDebuggerOnStart: true, flatten: true };
        cdp.onEvent = (msg) => {
          if (msg.method === "Target.attachedToTarget") {
            const worker = msg.params.sessionId;
            counts.set(worker, [0, 0]);
            cdp.send("Runtime.enable", {}, worker).catch(() => {});
            cdp.send("Runtime.evaluate", { expression: COUNT_MODULES }, worker).catch(() => {});
            cdp.send("Target.setAutoAttach", attach, worker).catch(() => {});
            cdp.send("Runtime.runIfWaitingForDebugger", {}, worker).catch(() => {});
          } else if (msg.method === "Runtime.consoleAPICalled" && counts.has(msg.sessionId)) {
            const m = String(msg.params.args[0]?.value).match(/^__jit (\d+) (\d+)/);
            if (m) {
              counts.set(msg.sessionId, [parseInt(m[1], 10), parseInt(m[2], 10)]);
            }
          }
        };
        await cdp.send("Target.setAutoAttach", attach, sessionId);
      }
      await cdp.send("Page.navigate", { url: QEMU_URL }, sessionId);
      for (;;) {
        await sleep(2000);
        text = await evaluate("typeof qemuText === 'function' ? qemuText() : ''");
        if (!extra.boot_ms && text.includes("== bench start")) {
          extra.boot_ms = Date.now() - started;
        }
        if (text.includes("== bench done")) {
          break;
        }
        if (Date.now() - started > 3600 * 1000) {
          throw new Error("no result after an hour:\n" + text.slice(-2000));
        }
      }
      if (countJit) {
        // (Reported every 50 modules, so up to 49 short per worker.)
        const all = [...counts.values()];
        extra.jit = {
          workers: all.length,
          compiling_workers: all.filter((c) => c[0] > 0).length,
          modules: all.reduce((sum, c) => sum + c[0], 0),
          bytes: all.reduce((sum, c) => sum + c[1], 0),
        };
      }
    }
    extra.hidden = await evaluate("document.hidden");
    cdp.close();
    return { ...extra, bench: parseBench(text.replace(/\r/g, "")) };
  } finally {
    chrome.kill();
    await sleep(500);
    if (!process.env.PROFILE_DIR) {
      fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5 });
    }
  }
}

(async () => {
  if (system === "sh") {
    const r = await oneRun();
    process.stdout.write(r.out.replace(/\r/g, ""));
    if (r.log) {
      console.error(r.log);
    }
    console.error(`[rc ${r.rc}, ${Math.round(r.ms)} ms]`);
    if (r.evaluated !== undefined) {
      console.log(JSON.stringify(r.evaluated, null, 1));
    }
    return;
  }
  const all = [];
  for (let i = 0; i < runs; i++) {
    const r = await oneRun();
    console.log(JSON.stringify({ system, run: i + 1, ...r }));
    all.push(r);
  }
  console.log("\n" + system + ", " + runs + " runs: median (min-max) ms, check");
  for (const name of Object.keys(all[0].bench)) {
    const times = all.map((r) => r.bench[name]?.[0]).filter((t) => t !== undefined).sort((a, b) => a - b);
    const checks = new Set(all.map((r) => r.bench[name]?.[1]));
    const median = times[Math.floor(times.length / 2)];
    console.log(`${name.padEnd(16)} ${String(median).padStart(8)} (${times[0]}-${times[times.length - 1]})  ` +
      (checks.size === 1 ? [...checks][0].slice(0, 16) : "CHECKS DIFFER " + [...checks].join(",")));
  }
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
