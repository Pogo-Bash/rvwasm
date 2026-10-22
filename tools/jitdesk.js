#!/usr/bin/env node
// The gate before a deploy: a dist's www (the page, runtime, kernel and boot image as the server would serve them) in
// Chrome for Testing, booted as a visitor boots it (the default page), then the emulated Konsole, Dolphin and Kate
// started the way the page's launcher starts them, Konsole again (the store of translations should serve it), and a look
// at whether any Worker failed and the window server and display are still alive. The disks come from the live site
// (their manifests and pieces are the same as the dist's), so is anything else the dist lacks; the relay's WebSocket is
// not proxied (no network in the machine).
//
//   tools/with-browser.sh WHO node tools/jitdesk.js DIST/www [--url https://plasmaontheweb.com] [--query '?x=1'] [--shots DIR]
//                         [--overlay DIR] [--private] [--quota-mb N] [--remove-storage-after N]
//
// The storage options (tests/storage-chrome.md): --overlay DIR serves the files of DIR in place of the dist's (the three
// of tools/www that the storage work changed, say); --private runs the page in a browser context of its own, an
// off-the-record one like a private or Guest window (a file system in memory, removed with the data; the default profile's
// is on disk); --quota-mb N caps the origin's storage at N MiB (Storage.overrideQuotaForOrigin; navigator.storage.estimate()
// does not show it); --remove-storage-after N removes the origin's file systems (Storage.clearDataForOrigin, what the
// browser's own removal looks like to the page) after the N-th program has its window, then the rest are launched and
// the page's note about it is looked for.
//
// Exit 0 if every program got a window and nothing failed, else 1. CHROME=path of another Chrome.

const { spawn } = require("child_process");
const fs = require("fs");
const http = require("http");
const https = require("https");
const os = require("os");
const path = require("path");

if (process.env.RV_BROWSER != "1") {
  console.error("run this under tools/with-browser.sh (one test browser at a time)");
  process.exit(2);
}
const args = process.argv.slice(2);
const www = path.resolve(args[0] || "dist/www");
const opt = (name, def) => (args.includes(name) ? args[args.indexOf(name) + 1] : def);
const upstream = new URL(opt("--url", "https://plasmaontheweb.com"));
const query = opt("--query", "");
const shots = opt("--shots", "");
const overlay = opt("--overlay", "") ? path.resolve(opt("--overlay", "")) : "";
const private_context = args.includes("--private");
const quota_mb = Number(opt("--quota-mb", 0));
const remove_after = Number(opt("--remove-storage-after", 0));
const CHROME = process.env.CHROME || path.join(__dirname,
  "chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const types = { ".js": "text/javascript", ".html": "text/html", ".css": "text/css", ".wasm": "application/wasm", ".json": "application/json", ".svg": "image/svg+xml", ".gz": "application/gzip" };

// A hook for the test only (the file the server serves is not the dist's): launch a program of the launcher by name.
const LAUNCH_ANCHOR = "    order: (op, id, a = 0, b = 0, c = 0) => order(op, id, a, b, c),";
const LAUNCH_HOOK = LAUNCH_ANCHOR + `
    appNames: () => { try { const v = new DataView(os.memory.buffer), t = table.address;
      const at = v.getUint32(t + H_APPS_LO, true) + v.getUint32(t + H_APPS_HI, true) * 0x100000000;
      return JSON.parse(decoder.decode(new Uint8Array(os.memory.buffer, at, v.getUint32(t + H_APPS_LEN, true)).slice())).map((x) => x.name); } catch (e) { return []; } },
    launchByName: (n) => { const names = window.rvDisplay.appNames(); const i = names.findIndex((x) => x.toLowerCase().includes(String(n).toLowerCase()));
      if (i >= 0) order(ORDER_LAUNCH, 0, i, new DataView(os.memory.buffer).getUint32(table.address + H_APPS_SERIAL, true) | 0); return i; },`;

const server = http.createServer((req, res) => {
  const p = decodeURIComponent(req.url.split("?")[0]);
  let file = path.join(www, p == "/" || p == "/desktop" ? "dev.html" : p);
  if (overlay) {
    const mine = path.join(overlay, p == "/" || p == "/desktop" ? "dev.html" : p);
    if (mine.startsWith(overlay) && fs.existsSync(mine) && fs.statSync(mine).isFile()) file = mine;
  }
  const headers = { "Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp", "Cache-Control": "no-store" };
  if ((file.startsWith(www) || (overlay && file.startsWith(overlay))) && fs.existsSync(file) && fs.statSync(file).isFile()) {
    let body = fs.readFileSync(file);
    if (p == "/display.js") {
      const text = body.toString("utf8");
      if (!text.includes(LAUNCH_ANCHOR)) console.error("jitdesk: display.js has no place for the launch hook");
      body = Buffer.from(text.replace(LAUNCH_ANCHOR, LAUNCH_HOOK));
    }
    res.writeHead(200, { ...headers, "Content-Type": types[path.extname(file)] || "application/octet-stream", "Content-Length": body.length });
    res.end(body);
    return;
  }
  const up = https.request({ host: upstream.host, path: req.url, method: req.method,
    headers: { host: upstream.host, "accept-encoding": "identity", accept: req.headers.accept || "*/*", ...(req.headers.range ? { range: req.headers.range } : {}), "user-agent": req.headers["user-agent"] || "jitdesk" } }, (r) => {
    res.writeHead(r.statusCode, { ...r.headers, ...headers });
    r.pipe(res);
  });
  up.on("error", (e) => { console.error("jitdesk: proxy " + req.url + ": " + e.message); res.writeHead(502); res.end(); });
  req.pipe(up);
});
server.on("upgrade", (req, socket) => socket.destroy());

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
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "jitdesk-"));
  const { chrome, address } = await startChrome(profile);
  const problems = [], errors = [];
  const t00 = Date.now();
  const stamp = () => ((Date.now() - t00) / 1000).toFixed(1) + "s";
  let ok = false, diag = null;
  try {
    const cdp = await connect(address);
    const context = private_context ? (await cdp.send("Target.createBrowserContext", {})).browserContextId : undefined;
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank", browserContextId: context });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    cdp.onEvent = (msg) => {
      if (msg.method == "Inspector.targetCrashed" || msg.method == "Target.targetCrashed") problems.push("the page crashed at " + stamp());
      if (msg.method == "Runtime.exceptionThrown") errors.push(stamp() + " exception: " + (msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text).slice(0, 200));
      if (msg.method == "Runtime.consoleAPICalled" && msg.params.type == "error") errors.push(stamp() + " console.error: " + msg.params.args.map((a) => a.value ?? a.description ?? "").join(" ").slice(0, 200));
      if (msg.method == "Log.entryAdded" && msg.params.entry.level == "error") errors.push(stamp() + " log: " + msg.params.entry.text.slice(0, 160) + " " + (msg.params.entry.url || ""));
    };
    if (quota_mb) {
      await cdp.send("Storage.overrideQuotaForOrigin", { origin: base, quotaSize: quota_mb * 1048576 }, sessionId);
      console.log("storage quota of " + base + " capped at " + quota_mb + " MiB");
    }
    await cdp.send("Page.enable", {}, sessionId);
    await cdp.send("Runtime.enable", {}, sessionId);
    await cdp.send("Log.enable", {}, sessionId);
    await cdp.send("Inspector.enable", {}, sessionId).catch(() => {});
    // Workers of the machine: their errors come to the page as events the runtime logs; also look at the targets.
    const evaluate = async (expression) => {
      const r = await cdp.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId);
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      return r.result.value;
    };
    diag = evaluate;
    const poll = async (what, expression, seconds) => {
      const until = Date.now() + seconds * 1000;
      while (Date.now() < until) {
        if (problems.length) return null;
        const v = await evaluate(expression).catch(() => null);
        if (v) return v;
        await sleep(1000);
      }
      problems.push("timed out waiting for " + what + " after " + seconds + " s (at " + stamp() + ")");
      return null;
    };
    const logBad = "(typeof linuxLog != 'undefined' ? linuxLog : []).filter((l) => /Wasm crash|Worker error|heartbeat|null function|signal handler crash|null page|InternalError|RuntimeError|LinkError/i.test(l))";
    console.log("booting " + base + "/dev.html" + query);
    await cdp.send("Page.navigate", { url: base + "/dev.html" + query }, sessionId);
    const apps = await poll("the launcher's programs", "window.rvDisplay && rvDisplay.appNames && rvDisplay.appNames().length ? rvDisplay.appNames() : null", 420);
    if (!apps) throw new Error("no launcher");
    console.log("[" + stamp() + "] desktop up; " + apps.length + " programs: " + apps.slice(0, 12).join(", "));
    await sleep(15000);
    const windows = () => evaluate("rvDisplay.windows().length");
    console.log("[" + stamp() + "] windows at start: " + await windows() + "; " + JSON.stringify(await evaluate("rvDisplay.windows().map((w) => [w.kind, w.role, w.title || w.app_id || ''])")).slice(0, 300));
    const shot = async (name) => {
      if (!shots) return;
      fs.mkdirSync(shots, { recursive: true });
      const data = await evaluate("rvDisplay.shot()").catch(() => null);
      if (data) fs.writeFileSync(path.join(shots, name + ".png"), Buffer.from(data.split(",")[1], "base64"));
    };
    await shot("desktop");
    const stat0 = await evaluate("linuxOs.jit ? linuxOs.jit.stats() : {}");
    const info0 = await evaluate("jitcache.info(linuxJitCache)").catch(() => null);
    const results = [];
    for (const name of ["Konsole", "Dolphin", "Kate", "Konsole"]) {
      const n0 = await windows();
      const t0 = Date.now();
      const idx = await evaluate(`rvDisplay.launchByName(${JSON.stringify(name)})`);
      if (idx < 0) { problems.push(name + ": not in the launcher"); break; }
      const got = await poll(name + "'s window", `rvDisplay.windows().length > ${n0}`, 400);
      const secs = (Date.now() - t0) / 1000;
      await sleep(8000);
      await shot((results.filter((r) => r.name == name).length ? name + "2" : name).toLowerCase());
      const stats = await evaluate("linuxOs.jit ? linuxOs.jit.stats() : {}");
      const bad = await evaluate(logBad);
      results.push({ name, window: !!got, seconds: +secs.toFixed(1), windows_now: await windows(), bad_log_lines: bad.length, jit_hits_total: stats.hits, jit_compiles_total: stats.compiles });
      console.log("[" + stamp() + "] " + JSON.stringify(results[results.length - 1]));
      if (!got) break;
      if (remove_after && results.length == remove_after) {
        console.log("[" + stamp() + "] removing the origin's file systems");
        await cdp.send("Storage.clearDataForOrigin", { origin: base, storageTypes: "file_systems" }, sessionId);
        // The disks find out when they are next asked or when idle (5 s), the page's look is every 10 s.
        const noted = await poll("the page's note about the storage", "(document.getElementById('storagenote') || { hidden: true }).hidden === false", 60);
        console.log("[" + stamp() + "] the note: " + (noted ? JSON.stringify(await evaluate("document.getElementById('storagenote-text').textContent")) : "NOT SHOWN"));
        if (typeof jitcache != "undefined") await evaluate("jitcache.save_now ? jitcache.save_now() : 0").catch(() => {});
      }
    }
    const storage_lines = await evaluate("linuxLog.filter((l) => /\\[Disk |\\[JIT cache\\]|\\[Storage\\]/.test(l) && !/saved,|kept from before|fetching ahead|new:/.test(l)).slice(-30)").catch(() => []);
    const disk_error_lines = await evaluate("linuxLog.filter((l) => /^\\[Disk [^\\]]*\\]: error:/.test(l)).length").catch(() => -1);
    const lost_lines = await evaluate("linuxLog.filter((l) => /removed this site's storage/.test(l)).length").catch(() => -1);
    const note_shown = await evaluate("(document.getElementById('storagenote') || { hidden: true }).hidden === false").catch(() => false);
    console.log("storage-related lines of the machine's log (last 30):\n  " + storage_lines.join("\n  "));
    console.log("disk 'error:' lines: " + disk_error_lines + "; lines about lost storage: " + lost_lines + "; note shown: " + note_shown);
    // Is the display still alive? (frames still arrive) and the machine's log.
    const b0 = await evaluate("rvDisplay.stats.frames || 0");
    await sleep(5000);
    const b1 = await evaluate("rvDisplay.stats.frames || 0");
    const finalBad = await evaluate(logBad);
    const info1 = await evaluate("jitcache.info(linuxJitCache)").catch(() => null);
    const stat1 = await evaluate("linuxOs.jit ? linuxOs.jit.stats() : {}");
    console.log("jit stats from before the programs to the end: modules hit " + (stat1.hits - (stat0.hits || 0)) + ", compiled " + (stat1.compiles - (stat0.compiles || 0)) +
                ", bytes hit " + (stat1.bytes_hit - (stat0.bytes_hit || 0)) + ", bytes compiled " + (stat1.bytes_compiled - (stat0.bytes_compiled || 0)));
    console.log("store before: " + JSON.stringify(info0) + "\nstore after:  " + JSON.stringify(info1));
    console.log("display frames in 5 s at the end: " + (b1 - b0));
    console.log("problem lines in the machine's log: " + finalBad.length + (finalBad.length ? "\n  " + finalBad.slice(0, 8).join("\n  ") : ""));
    console.log("page errors (" + errors.length + "):" + (errors.length ? "\n  " + errors.slice(0, 15).join("\n  ") : ""));
    const programs = results.filter((r) => r.window).length;
    ok = programs == 4 && !problems.length && !finalBad.length && b1 > b0 && !errors.some((e) => /Wasm crash|RuntimeError|LinkError|Worker error|Uncaught|InternalError/i.test(e));
    if (remove_after) {
      // after the removal: the note was shown, and the disks did not flood the log with errors
      const storage_ok = note_shown && disk_error_lines >= 0 && disk_error_lines <= 5 && lost_lines >= 1;
      if (!storage_ok) console.log("storage check FAILED: note " + note_shown + ", error lines " + disk_error_lines + ", lost lines " + lost_lines);
      ok = ok && storage_ok;
    }
    console.log("windows for " + programs + " of 4 launches; problems: " + (problems.length ? problems.join("; ") : "none"));
    cdp.close();
  } catch (e) {
    console.error("jitdesk: " + e.message);
    try {
      console.error("the machine's log, last lines:\n" + (await diag("(typeof linuxLog != 'undefined' ? linuxLog : ['no linuxLog']).slice(-40).join('\\n')")));
      console.error("display: " + await diag("JSON.stringify({ rvDisplay: typeof rvDisplay, active: window.rvDisplay && rvDisplay.active && rvDisplay.active(), stats: window.rvDisplay && rvDisplay.stats, windows: window.rvDisplay && rvDisplay.windows && rvDisplay.windows().length, apps: window.rvDisplay && rvDisplay.appNames && rvDisplay.appNames().length })"));
      console.error("errors so far: " + errors.slice(0, 12).join(" | "));
    } catch (e2) { console.error("(no diagnostics: " + e2.message + ")"); }
    if (problems.length) console.error("  " + problems.join("; "));
  } finally {
    chrome.kill();
    server.close();
    await sleep(1500);
    try { fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 500 }); } catch (e) { /* (left) */ }
  }
  console.log(ok ? "PASS" : "FAIL");
  process.exit(ok ? 0 : 1);
}
main();
