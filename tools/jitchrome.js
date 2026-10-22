#!/usr/bin/env node
// Launches of one program in one page of Chrome for Testing, then the page loaded again and the same launches
// again: wall time, the renderer's CPU (every Worker and V8's background threads), the share of batch modules the
// page already had, and the wasm code V8 has committed (tools/codemem.sh) after each. For the translation cache
// (tools/patch-runtime.py, 29; rv/jit.c, batches). Needs the dev server at :8000 only for the disk's pieces and the
// page's other files; the page, runtime, kernel and initramfs are this tree's own, built for node-machine (build/nm:
// build/nm/vm-stage.sh), served by a small server of its own on a free port.
//
//   tools/with-browser.sh WHO node tools/jitchrome.js NMDIR 'GUEST-ENV' [--launches 3] [--visits 2] [--prog 'konsole -e true']
//
// GUEST-ENV: variables for the guest's command, e.g. 'RV_JIT_GROUP=8:15:64' (the old way of batching), or ''.
// Prints one JSON line per launch, then a table. CHROME=path of another Chrome. PROFILE_DIR keeps the profile.

const { spawn, execFileSync } = require("child_process");
const fs = require("fs");
const http = require("http");
const os = require("os");
const path = require("path");

if (process.env.RV_BROWSER != "1") {
  console.error("run this under tools/with-browser.sh (one test browser at a time)");
  process.exit(2);
}
const args = process.argv.slice(2);
const nm = path.resolve(args[0] || "build/nm");
const guestEnv = args[1] || "";
const opt = (name, def) => (args.includes(name) ? args[args.indexOf(name) + 1] : def);
const launches = parseInt(opt("--launches", "3"), 10);
const visits = parseInt(opt("--visits", "2"), 10);
const prog = opt("--prog", "konsole -e true");
const extraQuery = opt("--query", "");
const CHROME = process.env.CHROME || path.join(__dirname,
  "chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
const root = path.join(__dirname, "..");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// --- the server: this tree's files first, the dev server's for the rest (the disk's pieces, xterm, ...)
const own = new Map();
const addDir = (dir) => { for (const f of fs.readdirSync(dir)) { const p = path.join(dir, f); if (fs.statSync(p).isFile()) own.set("/" + f, () => fs.readFileSync(p)); } };
addDir(path.join(root, "tools/www"));
for (const f of ["linux.js", "linux-worker.js"]) own.set("/" + f, () => fs.readFileSync(path.join(nm, "rt", f)));
own.set("/vmlinux-wasm64_nommu.wasm", () => fs.readFileSync(path.join(nm, "vmlinux.wasm")));
own.set("/initramfs-wasm64_nommu.cpio.gz", () => Buffer.concat([fs.readFileSync(path.join(nm, "initramfs.cpio.gz")), fs.readFileSync(path.join(nm, "extra.cpio.gz"))]));
own.set("/rv.sha256", () => fs.readFileSync(path.join(nm, "rv.sha256")));
const types = { ".js": "text/javascript", ".html": "text/html", ".css": "text/css", ".wasm": "application/wasm", ".json": "application/json", ".svg": "image/svg+xml" };
const server = http.createServer((req, res) => {
  const p = req.url.split("?")[0];
  const headers = { "Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp", "Cache-Control": "no-store" };
  if (own.has(p)) {
    const body = own.get(p)();
    res.writeHead(200, { ...headers, "Content-Type": p.endsWith(".gz") ? "application/gzip" : types[path.extname(p)] || "application/octet-stream", "Content-Length": body.length });
    res.end(body);
    return;
  }
  const up = http.request({ host: "localhost", port: 8000, path: req.url, method: req.method, headers: { ...req.headers, host: "localhost:8000" } }, (r) => {
    res.writeHead(r.statusCode, { ...r.headers, ...headers });
    r.pipe(res);
  });
  up.on("error", () => { res.writeHead(502); res.end(); });
  req.pipe(up);
});

function startChrome(profile) {
  const chrome = spawn(CHROME, ["--headless=new", "--user-data-dir=" + profile, "--remote-debugging-port=0", "--no-first-run",
    "--no-default-browser-check", "--window-size=1100,1500", "--disable-background-timer-throttling",
    "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows", "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
  return new Promise((resolve, reject) => {
    let err = "";
    chrome.stderr.on("data", (d) => { err += d; const m = err.match(/DevTools listening on (ws:\/\/\S+)/); if (m) resolve({ chrome, address: m[1] }); });
    chrome.on("exit", () => reject(new Error("Chrome exited: " + err)));
  });
}
function connect(address) {
  const ws = new WebSocket(address);
  const waiting = new Map();
  const c = { onEvent: null };
  let next = 1;
  ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    if (msg.method == "Page.javascriptDialogOpening") ws.send(JSON.stringify({ id: next++, method: "Page.handleJavaScriptDialog", params: { accept: true }, sessionId: msg.sessionId }));
    if (!msg.id && c.onEvent) c.onEvent(msg);
    if (msg.id && waiting.has(msg.id)) {
      const { resolve, reject } = waiting.get(msg.id);
      waiting.delete(msg.id);
      msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
    }
  };
  return new Promise((resolve, reject) => {
    ws.onerror = () => reject(new Error("cannot connect to Chrome"));
    ws.onopen = () => resolve(Object.assign(c, {
      send(method, params = {}, sessionId) { const id = next++; ws.send(JSON.stringify({ id, method, params, sessionId })); return new Promise((res, rej) => waiting.set(id, { resolve: res, reject: rej })); },
      close: () => ws.close(),
    }));
  });
}

async function main() {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const base = "http://localhost:" + server.address().port;
  const profile = process.env.PROFILE_DIR || fs.mkdtempSync(path.join(os.tmpdir(), "jitchrome-"));
  fs.mkdirSync(profile, { recursive: true });
  const { chrome, address } = await startChrome(profile);
  let crashed = false;
  const rows = [];
  try {
    const cdp = await connect(address);
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    cdp.onEvent = (msg) => { if (msg.method == "Inspector.targetCrashed" || msg.method == "Target.targetCrashed") { crashed = true; console.error("THE PAGE CRASHED"); } };
    await cdp.send("Page.enable", {}, sessionId);
    await cdp.send("Inspector.enable", {}, sessionId).catch(() => {});
    const evaluate = async (expression) => {
      const r = await cdp.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId);
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      return r.result.value;
    };
    const procs = async () => (await cdp.send("SystemInfo.getProcessInfo")).processInfo.filter((p) => p.type == "renderer");
    const cpu = async () => (await procs()).reduce((a, p) => a + p.cpuTime, 0);
    const code = async () => {
      const ps = await procs();
      const big = ps.sort((a, b) => b.cpuTime - a.cpuTime)[0];
      try { return execFileSync(path.join(root, "tools/codemem.sh"), [String(big.id)], { encoding: "utf8", timeout: 120000 }).trim().split(" ")[0]; } catch (e) { return "?"; }
    };
    for (let visit = 1; visit <= visits; visit++) {
      const url = base + "/dev.html?disk=daily&home=off&jitshare=1" + (visit == 1 ? "&fresh-jitcache" : "") + (extraQuery ? "&" + extraQuery : "");
      await cdp.send("Page.navigate", { url }, sessionId);
      for (let i = 0; i < 150 && !await evaluate("typeof linuxReady != 'undefined'").catch(() => false); i++) await sleep(200);
      await evaluate("linuxReady");
      await sleep(3000);
      let prev = await evaluate("linuxOs.jit ? linuxOs.jit.stats() : {}");
      console.log(JSON.stringify({ visit, launch: 0, code_mib: await code(), kept_kib: prev.kept_kib }));
      for (let launch = 1; launch <= launches && !crashed; launch++) {
        const c0 = await cpu();
        const t0 = Date.now();
        const cmd = `cp /rv/launch2.sh /debian/root/; debian "${guestEnv} ROUNDS=1 PROG='${prog}' bash /root/launch2.sh"`;
        const r = await evaluate(`(async () => await linuxRun(${JSON.stringify(cmd)}, 400000))()`);
        const wall = (Date.now() - t0) / 1000;
        const c1 = await cpu();
        const st = await evaluate("linuxOs.jit ? linuxOs.jit.stats() : {}");
        const dh = (st.bytes_hit || 0) - (prev.bytes_hit || 0), dc = (st.bytes_compiled || 0) - (prev.bytes_compiled || 0);
        const mods = (st.hits + st.compiles) - ((prev.hits || 0) + (prev.compiles || 0));
        prev = st;
        const m = /round 1: rc (\S+), ([\d.]+) s, (\d+) regions, (\d+) from the store/.exec(r.out || "");
        const row = { visit, launch, guest_s: m ? +m[2] : null, rc: m ? m[1] : r.rc, regions: m ? +m[3] : null, from_store: m ? +m[4] : null,
          wall_s: +wall.toFixed(2), cpu_s: +(c1 - c0).toFixed(2), shared_pct: dh + dc ? +(100 * dh / (dh + dc)).toFixed(1) : 0, modules: mods,
          kept_kib: st.kept_kib, code_mib: await code() };
        if (!m) console.error("no round line in: " + JSON.stringify((r.out || "").slice(-300)));
        rows.push(row);
        console.log(JSON.stringify(row));
        if (/null function|signal handler crash|null page/.test(r.out || "")) console.log("CRASH TEXT in the output");
      }
      await evaluate("typeof jitcache != 'undefined' && jitcache.save_now ? jitcache.save_now().then(() => true) : false").catch(() => {});
      console.log("visit " + visit + " info: " + JSON.stringify(await evaluate("typeof jitcache != 'undefined' && linuxJitCache ? jitcache.info(linuxJitCache) : null").catch(() => null)));
    }
    cdp.close();
  } finally {
    chrome.kill();
    server.close();
    await sleep(1500);
    if (!process.env.PROFILE_DIR) try { fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 500 }); } catch (e) { /* (left in the temporary directory) */ }
  }
  console.log("\nvisit launch  wall_s  cpu_s  guest_s  shared%  modules  code_MiB");
  for (const r of rows) console.log([r.visit, r.launch, r.wall_s, r.cpu_s, r.guest_s, r.shared_pct, r.modules, r.code_mib].map((x) => String(x).padStart(7)).join(" "));
  if (crashed) process.exit(3);
}
main().catch((e) => { console.error(e); process.exit(1); });
