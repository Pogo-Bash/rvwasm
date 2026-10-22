#!/usr/bin/env node
// native-chrome.js for Firefox (headless, WebDriver BiDi on --remote-debugging-port; Marionette's chrome context for memory reports):
// boot the desktop page, start the native programs the way the launcher does (typed into the page's terminal), report the windows,
// every error the page and its Workers log, and the content process's memory (about:memory's reports: wasm code, JS, total) after each.
//   tools/with-browser.sh WHO node tests/wasmdyn/native-firefox.js --url http://127.0.0.1:PORT/dev.html?dl=1 \
//        [--apps konsole,dolphin,kate] [--ready-timeout 900] [--app-timeout 400] [--hold 240] [--shot DIR] [--profile DIR]
// --getnew: after System Settings is up, open Colors & Themes > Global Theme > Get New... and take a picture 90 s later (the list must show themes).
// --control FILE: after the programs are up, read commands appended to FILE and run them as they come (click X Y, dclick, type TEXT, key Enter, wait S,
//   shot NAME, wins, js EXPR, quit), answers in FILE.out; --control-max S (default 3600). The same commands as native-chrome.js --control.
// --via panel: start each program from Plasma's launcher (click, search, click) instead of typing the command into the console.
// --cmd TEMPLATE: the command typed for each program ({app}); default debian 'desktop wayland nqs-run {app}' (native); with ?dl=0 use debian 'desktop wayland {app}' (emulated).
// --log FILE: every console/log entry of the page and its Workers, appended (T+seconds level text).
// FIREFOX=path of the firefox binary (default: ~/dev/_tools/browsers/firefox/mac_arm-stable_157.0.1/Firefox.app/Contents/MacOS/firefox).
// Prints "T+s ..." lines on stderr and a JSON summary on the last line. --hold N: after the last program, keep all of them open N
// seconds, then report the memory and whether the windows are still there.
"use strict";
const { spawn, execSync } = require("child_process");
const fs = require("fs");
const net = require("net");
const os = require("os");
const path = require("path");
if (process.env.RV_BROWSER != "1") { console.error("run this under tools/with-browser.sh WHO node ..."); process.exit(2); }
const args = process.argv.slice(2);
const opt = (n, d) => (args.includes("--" + n) ? args[args.indexOf("--" + n) + 1] : d);
const url = opt("url"), apps = opt("apps", "konsole,dolphin,kate").split(",").filter((a) => a !== "none"), readyTimeout = Number(opt("ready-timeout", 900)), appTimeout = Number(opt("app-timeout", 400)), hold = Number(opt("hold", 0));
const via = opt("via", "terminal"), launchCmd = opt("cmd", "debian 'desktop wayland nqs-run {app}'"), logFile = opt("log"), gauge = args.includes("--gauge");
const execEvery = Number(opt("exec-every", 10)), controlFile = opt("control"), controlMax = Number(opt("control-max", 3600));
let lastExec = 0, dumpN = 0;
const shotDir = opt("shot"); if (shotDir) fs.mkdirSync(shotDir, { recursive: true });
const profile = opt("profile") || fs.mkdtempSync(path.join(os.tmpdir(), "rvff-")); fs.mkdirSync(profile, { recursive: true });
const FIREFOX = process.env.FIREFOX || path.join(os.homedir(), "dev/_tools/browsers/firefox/mac_arm-stable_157.0.1/Firefox.app/Contents/MacOS/firefox");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
fs.writeFileSync(path.join(profile, "user.js"), [
  'user_pref("remote.active-protocols", 3);', 'user_pref("marionette.port", 0);', 'user_pref("browser.shell.checkDefaultBrowser", false);',
  'user_pref("datareporting.policy.dataSubmissionEnabled", false);', 'user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);',
  'user_pref("browser.startup.homepage_override.mstone", "ignore");', 'user_pref("app.update.enabled", false);', 'user_pref("app.update.auto", false);',
  'user_pref("browser.tabs.warnOnClose", false);', 'user_pref("dom.disable_beforeunload", true);',
].join("\n") + "\n");
const ff = spawn(FIREFOX, ["--headless", "--no-remote", "--profile", profile, "--remote-debugging-port", "0", "--remote-allow-system-access", "--marionette", "about:blank"], { stdio: ["ignore", "pipe", "pipe"] });
let ffout = "";
const bidiAddress = new Promise((resolve, reject) => {
  const on = (d) => { ffout += d; const m = ffout.match(/WebDriver BiDi listening on (ws:\/\/\S+)/); if (m) resolve(m[1]); };
  ff.stdout.on("data", on); ff.stderr.on("data", on);
  ff.on("exit", () => reject(new Error("Firefox exited: " + ffout.slice(-2000))));
});
const rss = () => { try { const out = execSync("ps -axo rss=,command= | grep -F -- '" + profile + "' | grep -v grep", { encoding: "utf8" }); return out.split("\n").filter(Boolean).reduce((m, l) => Math.max(m, parseInt(l.trim(), 10) / 1024), 0); } catch (e) { return 0; } };
// Marionette (chrome context): the memory reporters of the whole browser, summed for the tab's content process
const marionette = async () => {
  let port = 0;
  for (let i = 0; i < 200 && !port; i++) { port = Number((ffout.match(/Listening on port (\d+)/) || [])[1] || (fs.existsSync(path.join(profile, "MarionetteActivePort")) ? fs.readFileSync(path.join(profile, "MarionetteActivePort"), "utf8") : 0)); if (!port) await sleep(100); }
  if (!port) return null;
  const sock = net.connect(port, "127.0.0.1"); let buf = Buffer.alloc(0); const waiting = []; let id = 0, hello = null;
  sock.on("data", (d) => { buf = Buffer.concat([buf, d]);
    for (;;) { const i = buf.indexOf(":"); if (i < 0) return; const n = Number(buf.slice(0, i).toString()); if (buf.length < i + 1 + n) return;
      const msg = JSON.parse(buf.slice(i + 1, i + 1 + n).toString()); buf = buf.slice(i + 1 + n);
      if (Array.isArray(msg) && msg[0] === 1) { const w = waiting.find((x) => x.id === msg[1]); if (w) { waiting.splice(waiting.indexOf(w), 1); msg[2] ? w.reject(new Error(JSON.stringify(msg[2]))) : w.resolve(msg[3]); } } else hello = msg; } });
  await new Promise((r) => sock.on("connect", r)); while (!hello) await sleep(50);
  const call = (name, params) => new Promise((resolve, reject) => { const mid = ++id; waiting.push({ id: mid, resolve, reject }); const j = JSON.stringify([0, mid, name, params || {}]); sock.write(j.length + ":" + j); });
  await call("WebDriver:NewSession", { capabilities: {} });
  await call("Marionette:SetContext", { value: "chrome" });
  return { call, close: async () => { try { await call("WebDriver:DeleteSession"); } catch (e) {} sock.destroy(); } };
};
// The engine's own account of executable memory: SpiderMonkey reserves ONE region of MaxCodeBytesPerProcess (2044 MiB,
// js/src/jit/ProcessExecutableMemory.cpp) per process, from which every wasm code segment, Ion and Baseline allocation comes,
// and the page cap is that region's size. On macOS the region shows up in vmmap as a 2.0G rwx/rwx VM_ALLOCATE; its RESIDENT +
// SWAPPED size is the pages committed so far (code is written into them, so committed == resident unless swapped).
// Returns {pid, reserve_mib, resident_mib, swapped_mib, dirty_mib, used_mib} for the child process that has the most, or null.
const unit = (t) => Number(t) * 16 / 1024;  // vmmap -pages: 16 KiB pages -> MiB
const execRegion = () => {
  let best = null;
  let pids = [];
  try { pids = execSync("ps -axo pid=,command= | grep -F -- '" + profile + "' | grep -v grep | grep -F plugin-container", { encoding: "utf8" }).split("\n").filter(Boolean).map((l) => parseInt(l.trim(), 10)); } catch (e) { return null; }
  for (const pid of pids) {
    let out = "";
    try { out = execSync("vmmap -pages " + pid + " 2>/dev/null | grep -E '^VM_ALLOCATE .*rwx/rwx'", { encoding: "utf8", timeout: 120000, maxBuffer: 1 << 26 }); } catch (e) { out = e.stdout ? String(e.stdout) : ""; }
    for (const line of out.split("\n")) {
      const m = line.match(/\[\s*([0-9]+)\s+([0-9]+)\s+([0-9]+)\s+([0-9]+)\s*\]/);
      if (!m) continue;
      const reserve = unit(m[1]); if (reserve < 2000 || reserve > 2100) continue;
      const r = { pid, reserve_mib: Math.round(reserve), resident_mib: Math.round(unit(m[2])), dirty_mib: Math.round(unit(m[3])), swapped_mib: Math.round(unit(m[4])) };
      r.used_mib = r.resident_mib + r.swapped_mib;
      if (!best || r.used_mib > best.used_mib) best = r;
    }
  }
  return best;
};
const MEM_SCRIPT = `const cb = arguments[arguments.length - 1];
const mgr = Cc["@mozilla.org/memory-reporter-manager;1"].getService(Ci.nsIMemoryReporterManager);
const rows = [];
mgr.getReports((process, path, kind, units, amount) => { rows.push([process, path, amount]); }, null, () => {
  const MiB = 1048576, procs = {};
  for (const [p, path, amount] of rows) { (procs[p] = procs[p] || []).push([path, amount]); }
  const out = {};
  for (const [p, list] of Object.entries(procs)) {
    const code = { wasm: 0, ion: 0, baseline: 0, regexp: 0, other: 0 }; let resident = 0, guard = 0, wasm_main = 0, wasm_workers = 0;
    for (const [path, amount] of list) {
      if (path === "resident") resident = amount;
      if (path === "wasm-guard-pages") guard = amount;
      let m = path.match(/^explicit\\/.*\\/non-heap\\/code\\/(wasm|ion|baseline|regexp|other)$/);
      if (m) { code[m[1]] += amount; if (m[1] === "wasm") { if (/^explicit\\/workers\\//.test(path)) wasm_workers += amount; else wasm_main += amount; } }
    }
    if (!(code.wasm + code.ion + code.baseline > 0)) continue;
    const tot = code.wasm + code.ion + code.baseline + code.regexp + code.other;
    out[p] = { resident_mib: Math.round(resident / MiB), code_total_mib: Math.round(tot / MiB), wasm_mib: Math.round(code.wasm / MiB), wasm_main_thread_mib: Math.round(wasm_main / MiB), wasm_workers_mib: Math.round(wasm_workers / MiB), ion_mib: Math.round(code.ion / MiB), baseline_mib: Math.round(code.baseline / MiB), other_mib: Math.round((code.regexp + code.other) / MiB), guard_pages_mib: Math.round(guard / MiB) };
  }
  cb(out); }, null, false);`;
(async () => {
  const BIDI = await bidiAddress;
  let ws = null; const waiting = new Map(); let next = 1; const events = []; let context = null;
  const onmsg = (event) => { const msg = JSON.parse(event.data);
    if (msg.id && waiting.has(msg.id)) { const w = waiting.get(msg.id); waiting.delete(msg.id); msg.type === "error" ? w.reject(new Error(msg.error + ": " + msg.message)) : w.resolve(msg.result); }
    else if (msg.method) events.push(msg); };
  const connect = async () => {
    for (let k = 0; ; k++) {
      ws = new WebSocket(BIDI + "/session"); ws.onmessage = onmsg;
      await new Promise((r) => (ws.onopen = r));
      try { await send("session.new", { capabilities: {} }); break; } catch (e) { if (k >= 30) throw e; try { ws.close(); } catch (x) {} await sleep(2000); }
    }
    await send("session.subscribe", { events: ["log.entryAdded", "browsingContext.load"] });
    context = (await send("browsingContext.getTree", {})).contexts[0].context;
  };
  const send = (method, params = {}) => { const id = next++; ws.send(JSON.stringify({ id, method, params })); return new Promise((resolve, reject) => waiting.set(id, { resolve, reject })); };
  await connect();
  const val = (r) => { const x = r && r.result; if (!x || r.type !== "success") return undefined; return x.type === "undefined" || x.type === "null" ? undefined : x.value; };
  const evaluate = async (expression, limit = 12000) => { try { const r = await Promise.race([send("script.evaluate", { expression, target: { context }, awaitPromise: true, resultOwnership: "none" }), sleep(limit).then(() => ({ type: "timeout" }))]); return val(r); } catch (e) { return undefined; } };
  const t0 = Date.now(), T = () => ((Date.now() - t0) / 1000).toFixed(1);
  const term = () => evaluate("[...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent.trimEnd()).filter(Boolean).slice(-30).join('\\n')");
  const errors = [], lines = []; let jit_oom = 0;
  const pump = () => { while (events.length) { const e = events.shift();
    if (e.method === "log.entryAdded" && /^FFDUMP /.test((e.params.text || ""))) { const parts = e.params.text.split(" "); fs.writeFileSync(path.join(process.env.FFDUMP_DIR || os.tmpdir(), "ffdump-" + parts[1] + "-" + (dumpN++) + ".wasm"), Buffer.from(parts[2], "base64")); continue; }
    if (e.method === "log.entryAdded" && logFile) { try { fs.appendFileSync(logFile, "T+" + T() + " " + e.params.level + " " + String(e.params.text || "").replace(/\n/g, " | ").slice(0, 1500) + "\n"); } catch (x) {} }
    if (e.method === "log.entryAdded") { const p = e.params; const text = (p.text || "").replace(/\n/g, " | ").slice(0, 400);
      if (/__rv_jit_install: InternalError: out of memory/.test(text)) { jit_oom++; continue; }
      if (/Running without ARB\/KHR robustness/.test(text)) continue;
      const bad = p.level === "error" || /Wasm crash|fatal|out of memory|RangeError|window server stopped|executable code|crash/i.test(text);
      if (/\[Dl\]|JIT cache|failed three times/.test(text) && p.level !== "error" && !/failed|crash/i.test(text)) continue;
      if (bad) { const line = "T+" + T() + " " + p.type + "." + p.level + " " + text + " (" + (((p.stackTrace || {}).callFrames || [{}])[0].url || "").replace(/^.*\//, "") + ":" + (((p.stackTrace || {}).callFrames || [{}])[0].lineNumber) + ")";
        if (/\/relay|favicon|jitcache.json|relay\/address|WebSocket connection|NS_ERROR_NET|CORS/.test(text) && !/Worker|worker/.test(text)) { lines.push(line); } else { console.error(line); errors.push(line); } } } } };
  const sh = async (label, cmds, n = 14) => { for (const c of cmds) { await evaluate("window.linuxOs.key_input(" + JSON.stringify(c + "\r") + "), 1"); await sleep(1200); } await sleep(1500); console.error("T+" + T() + " SH[" + label + "]:\n" + ((await term()) || "").split("\n").slice(-n).join("\n")); };
  // --via panel (BiDi input): where the 1024x768 screen is in the page, a click there, keys typed
  // (screen pixels of the window server's screen, as tests/wasmdyn/native-chrome.js uses them)
  const canvasPoint = async (x, y) => JSON.parse(await evaluate("(() => { const c = document.getElementById('screen'), r = c.getBoundingClientRect(); const s = window.rvDisplay && rvDisplay.active() ? rvDisplay.screenSize() : [c.width, c.height]; return JSON.stringify({ x: Math.round(r.left + (" + x + " + 0.5) * r.width / s[0]), y: Math.round(r.top + (" + y + " + 0.5) * r.height / s[1]) }); })()"));
  const click = async (x, y) => { const p = await canvasPoint(x, y);
    await send("input.performActions", { context, actions: [{ type: "pointer", id: "mouse", parameters: { pointerType: "mouse" }, actions: [
      { type: "pointerMove", x: p.x, y: p.y, origin: "viewport" }, { type: "pause", duration: 150 }, { type: "pointerDown", button: 0 }, { type: "pause", duration: 80 }, { type: "pointerUp", button: 0 }] }] }); };
  const typeText = async (text) => { for (const ch of text) await send("input.performActions", { context, actions: [{ type: "key", id: "kbd", actions: [{ type: "keyDown", value: ch }, { type: "pause", duration: 40 }, { type: "keyUp", value: ch }] }] }); };
  const shot = async (name) => { if (!shotDir) return; const r = await Promise.race([send("browsingContext.captureScreenshot", { context }), sleep(20000)]).catch(() => null); if (r && r.data) fs.writeFileSync(path.join(shotDir, name + ".png"), Buffer.from(r.data, "base64")); };
  const memory = async (label) => {
    const mem = { label, rss_mb: Math.round(rss()) };
    // The page's own account of the Modules it holds for the native programs: what the Workers compiled (a compile of a key that was
    // compiled before is a duplicate: in Firefox every one is its own copy of the code) and what the page keeps.
    mem.dl = await evaluate("(() => { const l = (window.linuxOs && window.linuxOs.dl && window.linuxOs.dl.log) || []; const by = {}; let compiles = 0, dupBytes = 0, uniqBytes = 0, preload = 0, shared = 0; for (const e of l) { if (e.how == 'compile') { compiles++; if (by[e.key]) dupBytes += e.size; else { by[e.key] = 1; uniqBytes += e.size; } } else if (e.how == 'preload') preload++; else if (e.how == 'shared') shared++; } const j = window.linuxOs && window.linuxOs.jit ? Object.assign({ account: window.linuxOs.jit_code }, window.linuxOs.jit.stats()) : null; const big = Object.entries(by).length ? l.filter((e) => e.how == 'compile' || e.how == 'preload').sort((a, b) => b.size - a.size).slice(0, 8).map((e) => Math.round(e.size / 104857.6) / 10 + 'MB ' + e.how) : []; return JSON.stringify({ code_space_retries: (window.linuxLog || []).filter((x) => /no code space/.test(x)).length, biggest: big, kept_modules: window.linuxOs.dl.keys().length, worker_compiles: compiles, duplicate_compile_mib: Math.round(dupBytes / 1048576 * 10) / 10, unique_compile_mib: Math.round(uniqBytes / 1048576 * 10) / 10, preloads: preload, shared_hits: shared, jit: j }); })()");
    mem.exec_code = execRegion();
    // --gauge: how much of the 2044 MiB is still free, found by asking the engine: tiny wasm modules cost 128 KiB of it each (two 64 KiB
    // blocks), so compile them until the engine says "out of memory" (at most 6000 = 750 MiB), count, and let them go. Briefly takes the
    // free code memory away from the machine's Workers: only at the memory snapshots, never during a launch.
    if (gauge) {
      // And the largest hole: a module with code of about 3-4 times its length is compiled at 2, 4, 8, ... MB of wasm; the code of a
      // module is ONE block of the engine's reservation, so a fragmented reservation fails a big one with plenty free in all.
      mem.largest_module_mb = await evaluate(`(async () => { const body = (reps, k0) => { const b = new Uint8Array(3 + reps * 8 + 1); let o = 0; b[o++] = 0; for (let i = 0; i < reps; i++) { b[o++] = 0x20; b[o++] = 0; b[o++] = 0x41; b[o++] = ((k0 + i) & 0x7f) | 0x80; b[o++] = 0x00; b[o++] = 0x6a; b[o++] = 0x21; b[o++] = 0; } b[o++] = 0x0b; return b.subarray(0, o); };
        const leb = (n) => { const o = []; do { let c = n & 0x7f; n >>>= 7; if (n) c |= 0x80; o.push(c); } while (n); return o; };
        const build = (mb) => { const per = 200000, nf = Math.max(1, Math.round(mb * 1048576 / (per * 8 / 8 * 1))); const bodies = []; for (let f = 0; f < nf; f++) bodies.push(body(Math.floor(per / 8), f)); const parts = []; const push = (a) => parts.push(Uint8Array.from(a)); push([0,0x61,0x73,0x6d,1,0,0,0]); push([1,5,1,0x60,1,0x7f,0]);
          push([3, ...leb(1 + nf), ...leb(nf), ...new Array(nf).fill(0)].slice(0,0)); const tsec = [...leb(nf), ...new Array(nf).fill(0)]; push([3, ...leb(tsec.length), ...tsec]);
          const lens = bodies.map((b) => leb(b.length).length + b.length); const csz = leb(nf).length + lens.reduce((a, b) => a + b, 0); push([10, ...leb(csz), ...leb(nf)]); for (const b of bodies) { push(leb(b.length)); parts.push(b); }
          const total = parts.reduce((a, p) => a + p.length, 0); const out = new Uint8Array(total); let at = 0; for (const p of parts) { out.set(p, at); at += p.length; } return out; };
        let best = 0; for (const mb of [2, 4, 8, 16, 32, 64]) { try { const m = await WebAssembly.compile(build(mb)); best = mb; } catch (e) { break; } } return best; })()`, 120000);

      mem.free_code_mib = await evaluate(`(() => { const sec = (id, b) => [id, b.length, ...b]; const keep = []; let n = 0; try { for (; n < 6000; n++) { const k = n & 0x3fff; keep.push(new WebAssembly.Module(new Uint8Array([0,0x61,0x73,0x6d,1,0,0,0, ...sec(1,[1,0x60,1,0x7f,0]), ...sec(3,[1,0]), ...sec(7,[1,1,0x66,0,0]), ...sec(10,[1, 9, 0, 0x20, 0, 0x41, (k & 0x7f) | 0x80, (k >> 7) & 0x7f, 0x6a, 0x1a, 0x0b].slice(0)) ]))); } } catch (e) {} const r = n * 128 / 1024; keep.length = 0; return r; })()`);
    }
    console.error("T+" + T() + " EXEC CODE " + label + ": " + JSON.stringify(mem.exec_code) + (gauge ? " LARGEST COMPILED MODULE " + mem.largest_module_mb + " MB of wasm; FREE (by tiny-module gauge) " + mem.free_code_mib + " MiB" + (mem.free_code_mib >= 750 ? " or more" : "") : "") + " dl: " + mem.dl);
    try { await send("session.end", {}); } catch (e) {} try { ws.close(); } catch (e) {} await sleep(500);
    let mn = null;
    try { mn = await marionette(); const r = await Promise.race([mn.call("WebDriver:ExecuteAsyncScript", { script: MEM_SCRIPT, args: [] }), sleep(90000).then(() => { throw new Error("timeout"); })]); mem.reports = r.value; } catch (e) { mem.error = e.message; }
    if (mn) await mn.close();
    console.error("T+" + T() + " MEMORY " + JSON.stringify(mem).slice(0, 3500));
    if (gauge) await sleep(10000);  // (the probes' garbage goes before anything else is asked of the engine)
    await sleep(500); await connect();
    return mem;
  };
  setTimeout(() => { console.error("watchdog"); ff.kill("SIGKILL"); process.exit(3); }, (readyTimeout + apps.length * appTimeout + hold + 400 + (controlFile ? controlMax : 0)) * 1000);
  // (the same window as native-chrome.js's, 1100x1500: Plasma's launcher is laid out for a tall screen; in Firefox's small default window it is a compact popup with its categories scrolled)
  await send("browsingContext.setViewport", { context, viewport: { width: 1100, height: 1500 } }).catch((e) => console.error("setViewport: " + e.message));
  await send("browsingContext.navigate", { context, url, wait: "none" });
  let panel = null, lastPeek = -100;
  while (panel === null && Number(T()) < readyTimeout) {
    pump();
    const n = await evaluate("(() => { const d = window.rvDisplay; return d && d.windows ? d.windows().length : -1; })()");
    if (typeof n == "number" && n >= 2) { panel = Number(T()); console.error("T+" + T() + " PANEL UP (" + n + " surfaces)"); }
    if (Number(T()) - lastPeek >= 45) { lastPeek = Number(T());
      const w = await evaluate("(() => { const d = window.rvDisplay; return d && d.windows ? JSON.stringify(d.windows().map((x) => [x.kind, x.appId, x.title, x.w, x.h])) : 'no rvDisplay'; })()");
      const tl = await evaluate("[...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent.trimEnd()).filter(Boolean).slice(-3).join(' | ')");
      console.error("T+" + T() + " peek: windows " + w + " rss " + Math.round(rss()) + " MB; terminal: " + (tl || "").slice(0, 200)); await shot("peek"); }
    if (Number(T()) - lastExec >= execEvery) { lastExec = Number(T()); const x = execRegion(); console.error("T+" + T() + " exec code " + (x ? x.used_mib + " MiB (resident " + x.resident_mib + " + swapped " + x.swapped_mib + ")" : "?")); }
    await sleep(1000);
  }
  const summary = { ready_s: panel, apps: {}, memory: [], peak_rss_mb: 0, errors_total: 0 };
  if (panel === null) console.error("the desktop was not ready in " + readyTimeout + " s");
  else {
    await sleep(15000); pump();
    for (const ex of ["location.search", "navigator.userAgent", "typeof rv_dl_wanted == 'function' ? rv_dl_wanted(location.search, navigator.userAgent) : 'no rv_dl_wanted'", "window.linuxOs && window.linuxOs.dl ? window.linuxOs.dl.enabled + ' keys ' + window.linuxOs.dl.keys().length : 'no dl'"])
      console.error("T+" + T() + " eval " + ex.slice(0, 40) + " => " + (await evaluate(ex)));
    await sh("before", ["clear", "echo CPUS; cat /sys/devices/system/cpu/online; echo KTHREADS; ps | grep -c '\\['", "grep nq-shared /proc/mounts; ls /sys/block | tr '\\n' ' '; free -m | head -2; echo"], 10);
    summary.memory.push(await memory("panel"));
    for (const app of apps) {
      const before = errors.length;
      const wins0 = await evaluate("(() => { const d = window.rvDisplay; return d && d.windows ? d.windows().filter((w) => w.kind === 1 || w.appId).length : -1; })()");
      const t1 = Number(T()); console.error("T+" + T() + " LAUNCH " + app + " (windows before: " + wins0 + ")");
      if (via === "panel") {
        // as a person does (the program is then a child of plasmashell): the panel's pinned icon, or the launcher button, the category and the program
        // (the same positions as native-chrome.js: Plasma's panel and Kickoff popup are found in the window server's table)
        const PANEL = { systemsettings: 1, dolphin: 2 }, MENU = { konsole: ["System", 2], kate: ["Development", 0], okular: ["Office", 0], gwenview: ["Graphics", 2], ark: ["Utilities", 0] };
        const CATEGORY = { Development: 148, Graphics: 218, Office: 323, System: 358, Utilities: 393 };
        const wins = async () => JSON.parse((await evaluate("JSON.stringify(rvDisplay.windows().map((w) => ({ kind: w.kind, layer: w.layer, x: w.x, y: w.y, w: w.w, h: w.h, appId: w.appId })))")) || "[]");
        const pw = (await wins()).find((w) => w.kind === 6 && w.layer === 2);
        if (PANEL[app]) { await click(pw.x + 33 + 49 * PANEL[app], pw.y + pw.h / 2); }
        else {
          await click(pw.x + 33, pw.y + pw.h / 2); await sleep(5000);
          const pop = (await wins()).filter((w) => w.kind === 1 && w.appId === "org.kde.plasmashell").pop();
          console.error("T+" + T() + " popup at " + JSON.stringify(pop)); await shot("popup-" + app);
          await click(pop.x + 70, pop.y + CATEGORY[MENU[app][0]]);
          for (let i = 0; i < 30; i++) { await sleep(1000); const b = await evaluate("(async () => { let u = rvDisplay.shot(); if (u && u.then) u = await u; const img = new Image(); img.src = u; await img.decode(); const c = document.createElement('canvas'); c.width = img.width; c.height = img.height; const g = c.getContext('2d'); g.drawImage(img, 0, 0); const d = g.getImageData(" + (pop.x + 100) + ", " + (pop.y + 69) + ", 1, 1).data; return d[2] - d[0]; })()"); if (typeof b == "number" && b < 8) break; }
          await sleep(1500); await shot("menu-" + app);
          await click(pop.x + 302, pop.y + 72 + 41 * MENU[app][1]);
        }
      } else {
        await evaluate("window.linuxOs.key_input(" + JSON.stringify(launchCmd.replace(/\{app\}/g, app) + " > /tmp/" + app + ".out 2>&1 &\r") + "), 1");
      }
      await sleep(6000);
      let up = null, winapp = null;
      while (Number(T()) - t1 < appTimeout) {
        pump();
        const ws2 = await evaluate("(() => { const d = window.rvDisplay; return d && d.windows ? JSON.stringify(d.windows().filter((w) => w.appId).map((w) => w.appId + ':' + w.title)) : null; })()");
        if (ws2 && ws2.toLowerCase().includes(app) && !summary.apps[app]) { up = Number(T()) - t1; winapp = ws2; break; }
        if (errors.length > before + 3) break;
        summary.peak_rss_mb = Math.max(summary.peak_rss_mb, Math.round(rss()));
        if (Number(T()) - lastExec >= execEvery) { lastExec = Number(T()); const x = execRegion(); console.error("T+" + T() + " exec code " + (x ? x.used_mib + " MiB (resident " + x.resident_mib + " + swapped " + x.swapped_mib + ")" : "?")); }
        await sleep(500);
      }
      await sleep(8000); pump();
      await sh(app + "-after", ["clear", "echo CPUS; cat /sys/devices/system/cpu/online; ps | grep -E 'plasmashell|ld-wasm|" + app + "' | grep -v grep | while read pid rest; do echo PID $pid; grep Cpus_allowed_list /proc/$pid/status; done; tail -n 5 /tmp/" + app + ".out"], 16);
      const stopped = await evaluate("(() => { const m = [...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent).join('\\n'); return /window server stopped/.test(m) })()");
      const alive = await evaluate("(() => { const d = window.rvDisplay; return d && d.active ? d.active() : null })()");
      if (up === null) {
        const g = await evaluate("JSON.stringify({ log: (window.linuxLog || []).slice(-80), out: (typeof output == 'string' ? output.slice(-2500) : null) })");
        try { const j = JSON.parse(g || "{}"); console.error("T+" + T() + " GUEST LOG (no window for " + app + "):\n" + (j.log || []).join("\n").slice(-6000) + "\n--- console output tail:\n" + (j.out || "")); } catch (e) { console.error("guest log unavailable: " + g); }
      }
      summary.apps[app] = { window_after_s: up, windows: winapp, new_errors: errors.length - before, display_active: alive, window_server_stopped_text: stopped };
      console.error("T+" + T() + " " + app + ": " + JSON.stringify(summary.apps[app]));
      await shot(app);
      summary.memory.push(await memory("after " + app));
      // (the programs stay open: Konsole, Dolphin and Kate together is the point)
    }
    if (controlFile) {
      // commands from a file (as native-chrome.js --control): click X Y | dclick X Y | type TEXT | key Enter|Escape|Tab | wait S | shot NAME | wins | js EXPR | quit
      fs.writeFileSync(controlFile + ".out", ""); if (!fs.existsSync(controlFile)) fs.writeFileSync(controlFile, "");
      const out = (t) => fs.appendFileSync(controlFile + ".out", "T+" + T() + " " + t + "\n");
      const KEYS = { Enter: "", Escape: "", Tab: "", Backspace: "" };
      let done = 0, quit = false; console.error("T+" + T() + " CONTROL: reading " + controlFile);
      while (!quit && Number(T()) < controlMax) {
        pump();
        const cmds = fs.readFileSync(controlFile, "utf8").split("\n").filter((l) => l.trim());
        for (; done < cmds.length && !quit; done++) {
          const line = cmds[done], [c, ...a] = line.trim().split(/\s+/); const rest = line.trim().slice(c.length).trim();
          try {
            if (c === "click") { await click(Number(a[0]), Number(a[1])); out("ok " + line); }
            else if (c === "dclick") { await click(Number(a[0]), Number(a[1])); await sleep(120); await click(Number(a[0]), Number(a[1])); out("ok " + line); }
            else if (c === "pclick") { const r = JSON.parse((await evaluate("JSON.stringify((() => { const e = document.querySelector(" + JSON.stringify(rest) + "); if (!e) return null; const r = e.getBoundingClientRect(); return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) }; })())")) || "null");
              if (!r) out("error pclick: no element " + rest); else { await send("input.performActions", { context, actions: [{ type: "pointer", id: "mouse", parameters: { pointerType: "mouse" }, actions: [{ type: "pointerMove", x: r.x, y: r.y, origin: "viewport" }, { type: "pause", duration: 150 }, { type: "pointerDown", button: 0 }, { type: "pause", duration: 80 }, { type: "pointerUp", button: 0 }] }] }); out("ok " + line); } }   // (a DOM element of the page itself: the host-open prompt's buttons)
            else if (c === "targets") { const t = await send("browsingContext.getTree", {}); out(JSON.stringify(t.contexts.map((x) => x.url))); }   // (the browser's tabs)
            else if (c === "type") { await typeText(rest); out("ok " + line); }
            else if (c === "key") { await typeText(KEYS[a[0]] || a[0]); out("ok " + line); }
            else if (c === "wait") { for (let i = 0; i < Number(a[0]); i++) { await sleep(1000); if (i % 5 === 0) pump(); } out("ok " + line); }
            else if (c === "shot") { await shot(a[0]); out("ok shot " + a[0]); }
            else if (c === "wins") out(String(await evaluate("JSON.stringify(rvDisplay.windows().map((w) => ({ id: w.id, kind: w.kind, layer: w.layer, x: w.x, y: w.y, w: w.w, h: w.h, appId: w.appId, title: w.title })))")));
            else if (c === "js") out(String(await evaluate(rest)));
            else if (c === "quit") { quit = true; out("ok quit"); }
            else out("unknown command: " + line);
          } catch (e) { out("error " + line + ": " + e.message); }
        }
        await sleep(400);
      }
    }
    if (args.includes("--getnew") && summary.apps.systemsettings && summary.apps.systemsettings.window_after_s !== null) {
      // KNewStuff in System Settings (Colors & Themes > Global Theme > Get New...): the Global Themes list must show entries (not "No items found" / a providers error)
      await sleep(15000); await click(111, 279); await sleep(8000); await shot("getnew-0-page"); await click(898, 65);
      for (const s of [30, 30, 30]) { await sleep(s * 1000); pump(); }
      await shot("getnew-90s");
      summary.getnew = { windows: await evaluate("(() => { const d = window.rvDisplay; return d && d.windows ? JSON.stringify(d.windows().filter((w) => w.appId).map((w) => w.title)) : null; })()") };
      console.error("T+" + T() + " getnew: " + JSON.stringify(summary.getnew));
    }
    if (hold > 0) {
      const tH = Number(T());
      while (Number(T()) - tH < hold) { await sleep(10000); pump(); { const x = execRegion(); console.error("T+" + T() + " exec code " + (x ? x.used_mib + " MiB (resident " + x.resident_mib + " + swapped " + x.swapped_mib + ")" : "?")); } summary.peak_rss_mb = Math.max(summary.peak_rss_mb, Math.round(rss())); }
      const ws3 = await evaluate("(() => { const d = window.rvDisplay; return d && d.windows ? JSON.stringify(d.windows().filter((w) => w.appId).map((w) => w.appId)) : null; })()");
      summary.after_hold = { windows: ws3, display_active: await evaluate("(() => { const d = window.rvDisplay; return d && d.active ? d.active() : null })()"), new_errors: errors.length };
      await sh("after-hold", ["clear", "free -m | head -2; ps | grep -c ld-wasm"], 6);
      await shot("hold");
      summary.memory.push(await memory("after hold"));
    }
  }
  summary.errors_total = errors.length; summary.jit_install_oom = jit_oom; summary.peak_rss_mb = Math.max(summary.peak_rss_mb, Math.round(rss()));
  console.error("ERRORS:\n" + errors.join("\n"));
  console.error(JSON.stringify(summary));
  try { await send("browser.close"); } catch (e) {}
  await new Promise((r) => { ff.on("exit", r); setTimeout(() => { ff.kill("SIGKILL"); r(); }, 15000); });
  process.exit(0);
})().catch((e) => { console.error(e); ff.kill("SIGKILL"); process.exit(1); });
