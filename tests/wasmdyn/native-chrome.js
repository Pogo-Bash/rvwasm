#!/usr/bin/env node
// The native programs in Chrome for Testing, started the way people start them: boot the desktop page cold, wait for Plasma's panel, then
// click the program in Plasma's launcher (Kickoff: the launcher button, the category on the left, the program in the list; --typed: the name
// typed into its search and the first result, which loses keys now and then) or on the panel's pinned icons, and time each program's window. Also every error of every Worker (uncaught exception: message, file, line; console
// errors; the Worker's own exceptions through the DevTools protocol), the renderer's memory, and the bytes the site served.
//
//   tools/with-browser.sh WHO node tests/wasmdyn/native-chrome.js --url http://127.0.0.1:PORT/dev.html?disk=release[&dlbg=1] \
//        [--apps konsole,dolphin,systemsettings,kate,okular,gwenview,ark] [--via panel|terminal] [--budget 120] [--settle 60]
//        [--ready-timeout 600] [--shot DIR] [--profile DIR] [--site http://127.0.0.1:PORT] [--hide-after 0]
//        [--window 1100x1500] [--bar-shots] [--bar-stack]
//
// The gate (exit 0 or 1): every program has a window within --budget seconds of its click (default 120), no Worker threw and the page logged
// no crash line, the window server and the display are alive at the end, and every window that came up is still there 20 s after the last.
// --settle S: wait S seconds after the panel is up before the first click (the page's background job, ?dlbg=1, uses it).
// --via terminal: the old route (typed into the page's terminal: debian 'desktop wayland nqs-run PROGRAM'); the default is the panel.
// --hide-after S: S seconds after the first click, another tab is put in front for 90 s (the page is hidden; no background-throttling
// switches are given to the browser then), and the first tab is fronted again.
// --window WxH: the browser window's size (default 1100x1500). --bar-shots (needs --shot): the page's top bar (display.js bar, dev.html launch note):
// pictures of it idle, while each program starts ("Starting ..."), and right after its window shows ("Launched ..."), with the note's text in the log.
// --bar-stack: after the programs, three made-up launches through the page's own launch channel (nothing is asked of the machine): the bar with
// several starting at once, then each one done in turn, then after the "Launched" lines have faded.
// --control FILE: after the programs are up, read commands appended to FILE (one per line) and run them as they come, answers appended to FILE.out
//   (click X Y | dclick X Y | rclick X Y | cclick X Y (Ctrl held) | wheel X Y DY | type TEXT | key CODE | shot NAME | wait S | wins | js EXPR | quit;
//   page-level: pclick SELECTOR (a DOM element of the page, e.g. #rv-hostopen .rv-ho-primary) | pkey Enter|Escape|Tab | targets (the browser's page tabs)), X Y in the screen's pixels:
//   a browser that stays up between looks at the pictures (--control-max S: how long, default 3600), for dialogs whose coordinates are not known before.
// --site URL: a tests/wasmdyn/serve-local-site.py server; its /__stats (bytes by class) is read at every click and window.
// --profile DIR: kept, so a second visit has the first one's home disk, HTTP and code caches (omit for a cold visit).
// Prints "T+s ..." lines on stderr and a JSON summary on the last line.
"use strict";
const { spawn, execSync } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
if (process.env.RV_BROWSER != "1") { console.error("run this under tools/with-browser.sh WHO node ..."); process.exit(2); }
const args = process.argv.slice(2);
const opt = (n, d) => (args.includes("--" + n) ? args[args.indexOf("--" + n) + 1] : d);
const url = opt("url"), apps = opt("apps", "konsole,dolphin,systemsettings,kate").split(",").filter((a) => a !== "none"), readyTimeout = Number(opt("ready-timeout", 600));
const budget = Number(opt("budget", 120)), settle = Number(opt("settle", 15)), via = opt("via", "panel"), hideAfter = Number(opt("hide-after", 0));
const siteBase = opt("site"), dwell = Number(opt("dwell", 20)), controlFile = opt("control"), controlMax = Number(opt("control-max", 3600));
const shotDir = opt("shot"); if (shotDir) fs.mkdirSync(shotDir, { recursive: true });
const profile = opt("profile") || fs.mkdtempSync(path.join(os.tmpdir(), "rvnat-"));
const CHROME = process.env.CHROME || path.join(os.homedir(), "dev/rvwasm/tools/chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// What the launcher shows for each program, the window's app id, and where it starts (the panel's pinned icons, else the launcher's search).
const PROGRAMS = {
  konsole: { search: "konsole", id: "konsole", menu: ["System", 2] },
  dolphin: { panel: 2, search: "dolphin", id: "dolphin" },
  systemsettings: { panel: 1, search: "system settings", id: "systemsettings" },
  kate: { search: "kate", id: "kate", menu: ["Development", 0] },
  kwrite: { search: "kwrite", id: "kwrite", menu: ["Utilities", 4] },
  okular: { search: "okular", id: "okular", menu: ["Office", 0] },
  gwenview: { search: "gwenview", id: "gwenview", menu: ["Graphics", 2] },
  ark: { search: "archiving tool", id: "ark", menu: ["Utilities", 0] },
  kcalc: { search: "kcalc", id: "kcalc", menu: ["Utilities", 3] },
};
const throttle = args.includes("--keep-throttling");   // (only the hidden-tab run lets the browser throttle)
const flags = ["--headless=new", "--user-data-dir=" + profile, "--remote-debugging-port=0", "--no-first-run", "--no-default-browser-check", "--window-size=" + opt("window", "1100x1500").replace("x", ",")];
if (!hideAfter && !throttle) flags.push("--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows");
const chrome = spawn(CHROME, [...flags, "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
const address = new Promise((resolve, reject) => { let err = ""; chrome.stderr.on("data", (d) => { err += d; const m = err.match(/DevTools listening on (ws:\/\/\S+)/); if (m) resolve(m[1]); }); chrome.on("exit", () => reject(new Error("Chrome exited: " + err))); });
// The renderer's memory as macOS counts it (`footprint`: dirty plus compressed pages; ps's RSS drops when the system compresses pages and says nothing then)
const rss = () => { try { const out = execSync("ps -axo pid=,command= | grep -F 'user-data-dir=" + profile + "' | grep -v grep | grep -F 'type=renderer'", { encoding: "utf8" });
  return out.split("\n").filter(Boolean).reduce((m, l) => { const pid = parseInt(l.trim(), 10); let mb = 0;
    try { const f = execSync("footprint -p " + pid + " 2>/dev/null | grep -m1 'Footprint:'", { encoding: "utf8" }); const x = f.match(/Footprint: ([0-9.]+) ([KMG])B/); if (x) mb = Number(x[1]) * ({ K: 1 / 1024, M: 1, G: 1024 })[x[2]]; } catch (e) {}
    return Math.max(m, mb); }, 0); } catch (e) { return 0; } };
const siteStats = async () => { if (!siteBase) return null; try { return await (await fetch(siteBase + "/__stats")).json(); } catch (e) { return null; } };
(async () => {
  const ws = new WebSocket(await address);
  const waiting = new Map(); let next = 1; const events = [];
  ws.onmessage = (event) => { const msg = JSON.parse(event.data);
    if (msg.id && waiting.has(msg.id)) { const w = waiting.get(msg.id); waiting.delete(msg.id); msg.error ? w.reject(new Error(msg.error.message)) : w.resolve(msg.result); }
    else if (msg.method) events.push(msg); };
  await new Promise((r) => (ws.onopen = r));
  const send = (method, params = {}, sessionId) => { const id = next++; ws.send(JSON.stringify({ id, method, params, sessionId })); return new Promise((resolve, reject) => waiting.set(id, { resolve, reject })); };
  const { targetId } = await send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
  await send("Runtime.enable", {}, sessionId); await send("Page.enable", {}, sessionId);
  if (!hideAfter) await send("Emulation.setFocusEmulationEnabled", { enabled: true }, sessionId).catch(() => {});   // (the page counts as focused; with --hide-after it is the browser's own idea)
  // every Worker (and the Workers they start) is attached to as it starts: its exceptions and console errors come with its url
  const workers = new Map();
  await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: true, flatten: true, filter: [{ type: "worker" }] }, sessionId);
  const evaluate = async (expression) => { const r = await Promise.race([send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId), sleep(8000).then(() => ({ exceptionDetails: true }))]); return r.exceptionDetails ? undefined : r.result.value; };
  const t0 = Date.now(), T = () => ((Date.now() - t0) / 1000).toFixed(1);
  const say = (s) => console.error("T+" + T() + " " + s);
  const term = () => evaluate("[...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent.trimEnd()).filter(Boolean).slice(-30).join('\\n')");
  const sh = async (label, cmds, lines = 14) => {
    for (const c of cmds) { await evaluate("window.linuxOs.key_input(" + JSON.stringify(c + "\r") + "), 1"); await sleep(1200); }
    await sleep(1500);
    const t = (await term()) || "";
    say("SH[" + label + "]:\n" + t.split("\n").slice(-lines).join("\n"));
  };
  // The screen's pixels (the window server's) to the page's: the canvas is scaled to the viewport.
  const toClient = async (x, y) => JSON.parse(await evaluate("(() => { const c = document.getElementById('screen'), r = c.getBoundingClientRect(); const s = window.rvDisplay && rvDisplay.active() ? rvDisplay.screenSize() : [c.width, c.height]; return JSON.stringify({ x: r.left + (" + x + " + 0.5) * r.width / s[0], y: r.top + (" + y + " + 0.5) * r.height / s[1] }); })()"));
  const click = async (x, y) => { const p = await toClient(x, y);
    await send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y }, sessionId); await sleep(150);
    await send("Input.dispatchMouseEvent", { type: "mousePressed", x: p.x, y: p.y, button: "left", buttons: 1, clickCount: 1 }, sessionId); await sleep(80);
    await send("Input.dispatchMouseEvent", { type: "mouseReleased", x: p.x, y: p.y, button: "left", buttons: 0, clickCount: 1 }, sessionId); };
  // Keys go to the screen as tools/desktop-drive.js sends them (the screen's handler reads the key's code; no text): raw key down, a pause, key up,
  // one key at a time (a machine that is slow to take the release makes its key repeat double a letter).
  const key = async (type, code) => {
    await evaluate("(() => { const a = document.activeElement; if (!a || a == document.body) { document.getElementById('screen').focus(); } })()");
    await send("Input.dispatchKeyEvent", { type, code, key: code }, sessionId);
  };
  const stroke = async (code) => { await key("rawKeyDown", code); await sleep(70); await key("keyUp", code); await sleep(220); };
  // (letters, digits and the punctuation a command line or an address needs; shifted ones hold ShiftLeft)
  const PUNCT = { " ": ["Space"], ".": ["Period"], "/": ["Slash"], "-": ["Minus"], "=": ["Equal"], ";": ["Semicolon"], ",": ["Comma"], "'": ["Quote"],
    ":": ["Semicolon", 1], "_": ["Minus", 1], "?": ["Slash", 1], "&": ["Digit7", 1], "%": ["Digit5", 1], "#": ["Digit3", 1], "+": ["Equal", 1], "~": ["Backquote", 1], "\"": ["Quote", 1], "!": ["Digit1", 1], "|": ["Backslash", 1], ">": ["Period", 1], "<": ["Comma", 1] };
  const typeText = async (text) => {
    for (const ch of text) {
      let code = null, shift = false;
      if (PUNCT[ch]) { code = PUNCT[ch][0]; shift = !!PUNCT[ch][1]; }
      else if (/[0-9]/.test(ch)) code = "Digit" + ch;
      else if (/[a-z]/.test(ch)) code = "Key" + ch.toUpperCase();
      else if (/[A-Z]/.test(ch)) { code = "Key" + ch; shift = true; }
      else throw new Error("typeText: no key for " + JSON.stringify(ch));
      if (shift) { await key("rawKeyDown", "ShiftLeft"); await sleep(40); }
      await stroke(code);
      if (shift) { await key("keyUp", "ShiftLeft"); await sleep(40); }
    }
  };
  const press = async (name) => stroke(name);
  const displayWindows = async () => JSON.parse((await evaluate("(() => { const d = window.rvDisplay; return d && d.windows ? JSON.stringify(d.windows().map((w) => ({ id: w.id, kind: w.kind, layer: w.layer, x: w.x, y: w.y, w: w.w, h: w.h, appId: w.appId, title: w.title }))) : '[]'; })()")) || "[]");
  const barShots = args.includes("--bar-shots") && shotDir;
  // The page as it is, and the top strip of it at twice the size (the bar is a few pixels of the whole), and what the launch note says.
  const barShot = async (name) => {
    const note = await evaluate("(() => { const el = document.getElementById('launchnote'); return el && !el.hidden ? [...el.children].filter((c) => !c.hidden).map((c) => c.textContent + (c.classList.contains('ln-fade') ? ' [fading]' : '')).join(' || ') : ''; })()");
    const width = await evaluate("document.documentElement.clientWidth");
    const full = await Promise.race([send("Page.captureScreenshot", { format: "png" }, sessionId), sleep(10000)]).catch(() => null);
    if (full) fs.writeFileSync(path.join(shotDir, name + ".png"), Buffer.from(full.data, "base64"));
    const strip = await Promise.race([send("Page.captureScreenshot", { format: "png", clip: { x: 0, y: 0, width: width || 1100, height: 70, scale: 2 } }, sessionId), sleep(10000)]).catch(() => null);
    if (strip) fs.writeFileSync(path.join(shotDir, name + "-strip.png"), Buffer.from(strip.data, "base64"));
    say("BAR SHOT " + name + " (page width " + width + ", note: " + JSON.stringify(note || "") + ")");
    return note;
  };
  const errors = []; let ready = null; const lines = []; const notes = [];
  const handle = async (e) => {
    if (e.method === "Target.attachedToTarget") {
      const sid = e.params.sessionId, info = e.params.targetInfo; workers.set(sid, info.url);
      try { await send("Runtime.enable", {}, sid); await send("Log.enable", {}, sid); await send("Runtime.runIfWaitingForDebugger", {}, sid); } catch (x) {}
      try { await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }, sid); } catch (x) {}
    } else if (e.method === "Runtime.exceptionThrown") {
      const d = e.params.exceptionDetails; const who = e.sessionId === sessionId ? "page" : "worker " + (workers.get(e.sessionId) || "?").replace(/^.*\//, "");
      const text = ((d.exception && (d.exception.description || d.exception.value)) || d.text || "").replace(/\n/g, " | ").slice(0, 400);
      const line = "T+" + T() + " EXCEPTION [" + who + "] " + text + " (" + (d.url || "").replace(/^.*\//, "") + ":" + d.lineNumber + ")";
      console.error(line); errors.push(line);
    } else if (e.method === "Runtime.consoleAPICalled") {
      const text = e.params.args.map((a) => a.value !== undefined ? String(a.value) : (a.description || "")).join(" ");
      const who = e.sessionId === sessionId ? "page" : "worker " + (workers.get(e.sessionId) || "?").replace(/^.*\//, "");
      if (e.sessionId === sessionId) { pageLog.push("T+" + T() + " " + text.replace(/\n/g, " | ").slice(0, 240)); if (pageLog.length > 80) pageLog.shift(); }
      const m = text.match(/the desktop is ready after (\d+) ms/); if (m && ready === null) say("splash says the desktop is ready (" + m[1] + " ms)");
      if (/\[Dl\]|\[Launch\]|\[Warm\]|background compile/.test(text)) { const line = "T+" + T() + " . [" + who + "] " + text.replace(/\n/g, " | ").slice(0, 300); console.error(line); notes.push(line); }
      if (process.env.ALL && /Disk|disk|home|nqs|OPFS|storage/i.test(text)) console.error("T+" + T() + " . [" + who + "] " + text.replace(/\n/g, " | ").slice(0, 300));
      if (!/robustness in place/.test(text) && (e.params.type === "error" || /Wasm crash|fatal|out of memory|RangeError|window server stopped|failed|crash/i.test(text))) { const line = "T+" + T() + " console." + e.params.type + " [" + who + "] " + text.replace(/\n/g, " | ").slice(0, 400); console.error(line); lines.push(line); }
    } else if (e.method === "Log.entryAdded") { const en = e.params.entry; if (en.level === "error" && !/favicon|jitcache.json|\/relay|ws:\/\/|WebSocket|net::ERR/.test(en.text + (en.url || ""))) { const line = "T+" + T() + " log.error " + en.text.slice(0, 300) + " " + (en.url || ""); console.error(line); lines.push(line); } }
    else if (e.method === "Inspector.targetCrashed" || e.method === "Target.targetCrashed") { say("THE TAB CRASHED"); errors.push("tab crashed"); }
  };
  const pageLog = [];
  // The state of a machine that does not reach the desktop (J11): the kernel's own variables twice, a second apart (clock, runnable tasks per CPU,
  // each disk's request words), what the page's runners say, the last terminal lines and the page's last console lines.
  const stallDump = async () => {
    const read = `(async () => {
      const text = await (await fetch("kernel-symbols.txt", { cache: "no-store" })).text(); const at = {};
      for (const line of text.split("\\n")) { const p = line.split(" "); if (p.length == 3) at[p[2]] = parseInt(p[0], 16); }
      const os = window.linuxOs; if (!os || !os.memory) return { err: "no machine" };
      const look = () => { const m = new DataView(os.memory.buffer);
        const out = { jiffies: Number(m.getBigUint64(at.jiffies_64, true)), tick_cpu: m.getInt32(at.tick_do_timer_cpu, true), cpus: [], pages: os.memory.buffer.byteLength / 65536 };
        const online = m.getBigUint64(at.__cpu_online_mask, true);
        for (let cpu = 0; cpu < 64; cpu++) { if (!((online >> BigInt(cpu)) & 1n)) continue; const base = Number(m.getBigUint64(at.__per_cpu_offset + 8 * cpu, true)); out.cpus.push([cpu, m.getUint32(base + at.runqueues + 4, true)]); }
        out.disks = (os.disks || []).map((d) => d ? Array.from(d.slice(0, 9)) : null); return out; };
      const a = look(); await new Promise((r) => setTimeout(r, 1000)); const b = look();
      const r = os.runners || {}; const names = os.runner_names || {}; const run = [], idle = [];
      for (const k of Object.keys(r)) (r[k].running ? run : idle).push(k + ":" + (names[k] || "?"));
      return { a, b, running: run.slice(0, 40), nrunning: run.length, nidle: idle.length, switches_tail: JSON.stringify(os.switches.slice(-12)), crashes: os.kernel_crashes, jit_code: os.jit_code };
    })()`;
    const r = await evaluate(read);
    const term = await evaluate("[...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent.trimEnd()).filter(Boolean).slice(-8).join(' | ')");
    // The guest's own view, with shell builtins only (its sh has no vfork): every process's pid, name, state and wait channel, and the autostart log's end.
    const DS = "while read l; do echo DS $l; done < /debian/root/desktop-status.log; ", PS = "for p in /proc/[0-9]*; do read a b c d rest < $p/stat; case $b in *kworker*|*rcu*|*migration*|*cpuhp*|*ksoft*|*jbd2*|*wasm*) ;; *) echo PS $a $b $c $d;; esac; done; ";
    const probe = PS;
    const probe2 = "read a b c d < /proc/182/stat; echo A $a $b $c $d; read w < /proc/182/wchan; echo W $w; exec 4< /proc/182/status; n=0; while [ $n -lt 45 ]; do n=$((n+1)); read k v <&4 || break; case $k in State:|SigPnd:|ShdPnd:|SigBlk:|SigIgn:|SigCgt:|Threads:) echo ST $k $v;; esac; done; exec 4<&-";
    const probe3 = "kill -CHLD 182; echo SENTCHLD";
    const probe4 = PS + DS;
    let peSeq = 0; const ask = async (cmd) => { const pe = "PEND" + (++peSeq) + "X"; await evaluate("window.linuxOs.key_input(" + JSON.stringify("\u0004\u0004" + cmd + "; echo " + pe + "\r") + "), 1"); for (let i = 0; i < 15; i++) { await sleep(2000); const t = await evaluate("document.querySelector('.xterm-rows').textContent"); if ((t.split(pe).length - 1 >= 0 ? t.match(new RegExp(pe, "g")) || [] : []).length >= 2) break; } return await evaluate("[...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent.trimEnd()).filter(Boolean).slice(-120).join(' | ')"); };
    const guest = await ask(probe); const guest2 = "", guest3 = "", guest4 = "";
    return { kernel: r, guest, guest2, guest3, guest4, term, pageLog: pageLog.slice(-40) };
  };
  const pump = async () => { while (events.length) await handle(events.shift()); };
  const samples = []; let peakRss = 0;
  const sampleRss = () => { const r = Math.round(rss()); peakRss = Math.max(peakRss, r); samples.push([Number(T()), r]); return r; };
  const maxSeconds = readyTimeout + settle + apps.length * budget * 2 + 400 + hideAfter + (controlFile ? controlMax : 0);
  setTimeout(() => { console.error("watchdog"); chrome.kill("SIGKILL"); process.exit(3); }, maxSeconds * 1000);
  await send("Page.navigate", { url }, sessionId);
  // ready: Plasma's desktop and panel are layer surfaces of the window server (rvDisplay.windows(): at least two), not the splash's "desktop is ready"
  let panel = null, lastPeek = -100, panelWin = null;
  while (panel === null && Number(T()) < readyTimeout) {
    await pump();
    const wins = await displayWindows();
    if (wins.length >= 2) { panel = Number(T()); ready = panel; panelWin = wins.find((w) => w.kind === 6 && w.layer === 2) || wins[wins.length - 1]; say("PANEL UP (" + wins.length + " surfaces) rss " + sampleRss() + " MB"); }
    if (Number(T()) - lastPeek >= 45) {
      lastPeek = Number(T());
      const term = await evaluate("[...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent.trimEnd()).filter(Boolean).slice(-3).join(' | ')");
      say("peek: surfaces " + wins.length + " rss " + sampleRss() + " MB; terminal: " + (term || "").slice(0, 160));
    }
    await sleep(1000);
  }
  const summary = { gate: "FAIL", via, budget_s: budget, settle_s: settle, panel_s: ready, apps: {}, peak_rss_mb: 0, rss_at_panel_mb: 0, rss_before_first_click_mb: 0, errors_total: 0, log_error_lines: 0, still_there: null, site_bytes: null };
  if (ready === null) { say("the desktop was not ready in " + readyTimeout + " s"); summary.stall = await stallDump(); say("STALL (full dump in stall.json)"); if (shotDir) fs.writeFileSync(path.join(shotDir, "stall.json"), JSON.stringify(summary.stall, null, 1)); if (shotDir) { const r = await Promise.race([send("Page.captureScreenshot", { format: "png" }, sessionId), sleep(10000)]).catch(() => null); if (r) fs.writeFileSync(path.join(shotDir, "stall.png"), Buffer.from(r.data, "base64")); } }
  else {
    summary.rss_at_panel_mb = Math.round(rss());
    if (process.env.RV_STATUS_CAT) {
      // J11: the desktop script's own log of plasmashell starts and exits, read through the page's terminal.
      await sleep(Number(process.env.RV_STATUS_WAIT || 15000));
      await evaluate("window.linuxOs.key_input(" + JSON.stringify("cat /debian/root/desktop-status.log; echo PEND9X\r") + "), 1");
      for (let i = 0; i < 20; i++) { await sleep(3000); if ((await evaluate("document.querySelector('.xterm-rows').textContent")).includes("PEND9X")) { const n = (await evaluate("document.querySelector('.xterm-rows').textContent")).split("PEND9X").length; if (n >= 3) break; } }
      summary.status_log = await evaluate("[...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent.trimEnd()).filter((t) => /desktop:|PEND9X|No such/.test(t)).join(' | ')");
      say("STATUS " + summary.status_log);
    }
    // settle: the page's background job, if it is on, runs now; a sample of the memory and the site's bytes every 10 s
    if ((process.env.UX_SH || "")) { for (const c of (process.env.UX_SH || "").split(";;")) await sh("SH", ["clear; " + c], 28); }
    const tSettle = Number(T());
    while (Number(T()) - tSettle < settle) { await pump(); if (Math.round((Number(T()) - tSettle)) % 10 === 0) sampleRss(); await sleep(1000); }
    await pump();
    summary.rss_before_first_click_mb = Math.round(rss());
    if (barShots) await barShot("bar-idle");
    summary.site_before_first_click = await siteStats();
    for (const ex of ["location.search", "navigator.userAgent", "typeof rv_dl_wanted == 'function' ? rv_dl_wanted(location.search, navigator.userAgent) : 'no rv_dl_wanted'", "window.linuxOs && window.linuxOs.dl ? window.linuxOs.dl.enabled + ' modules ' + window.linuxOs.dl.keys().length : 'no dl'"]) {
      say("eval " + ex.slice(0, 40) + " => " + (await evaluate(ex)));
    }
    const geometry = async () => { const wins = await displayWindows(); const pw = wins.find((w) => w.kind === 6 && w.layer === 2) || panelWin;
      const pop = wins.filter((w) => w.kind === 1 && w.appId === "org.kde.plasmashell").pop(); return { pw, pop }; };
    const launcherButton = async () => { const { pw } = await geometry(); return [Math.round(pw.x + 33), Math.round(pw.y + pw.h / 2)]; };
    // The panel's pinned icons are 49 px apart from the launcher button (launcher, System Settings, file manager, browser).
    const panelIcon = async (n) => { const { pw } = await geometry(); return [Math.round(pw.x + 33 + 49 * n + (n > 0 ? 0 : 0)), Math.round(pw.y + pw.h / 2)]; };
    const startPanel = async (name) => {
      const p = PROGRAMS[name] || { search: name, id: name };
      if (p.panel) { const [x, y] = await panelIcon(p.panel); say("CLICK panel icon " + p.panel + " for " + name + " at " + x + "," + y); await click(x, y); return; }
      const [lx, ly] = await launcherButton(); say("CLICK launcher button for " + name); await click(lx, ly);
      let pop = null; for (let i = 0; i < 80 && !pop; i++) { await sleep(500); pop = (await geometry()).pop; }
      if (!pop) throw new Error("the launcher's popup did not open");
      await sleep(2500);
      if (p.menu && !args.includes("--typed")) {
        // The launcher's categories (Development, Graphics, ..., Utilities) are 35 px apart from the 148th px below the popup's top; a category's list
        // shows a second or three after the click, its rows are 41 px apart from the 72nd px. (Debian's menu: tools/x/xdg and the image.)
        const CATEGORY = { Development: 148, Graphics: 218, Office: 323, System: 358, Utilities: 393 };
        // (the list changes a second or three after the click, longer on a busy machine: the Favorites row is blue while it is the category shown)
        const favBlue = () => evaluate("(async () => { let u = rvDisplay.shot(); if (u && u.then) u = await u; const img = new Image(); img.src = u; await img.decode(); const c = document.createElement('canvas'); c.width = img.width; c.height = img.height; const g = c.getContext('2d'); g.drawImage(img, 0, 0); const d = g.getImageData(" + (pop.x + 100) + ", " + (pop.y + 69) + ", 1, 1).data; return d[2] - d[0]; })()");
        await click(pop.x + 70, pop.y + CATEGORY[p.menu[0]]);
        for (let i = 0; i < 30; i++) { await sleep(1000); const b = await favBlue(); if (typeof b == "number" && b < 8) break; }
        await sleep(1500);
        say("MENU " + p.menu[0] + ", row " + p.menu[1]);
        if (shotDir) { const r = await Promise.race([send("Page.captureScreenshot", { format: "png" }, sessionId), sleep(10000)]).catch(() => null); if (r) fs.writeFileSync(path.join(shotDir, name + "-menu.png"), Buffer.from(r.data, "base64")); }
        await click(pop.x + 302, pop.y + 72 + 41 * p.menu[1]);
        return;
      }
      await click(pop.x + 420, pop.y + 23);   // the search field
      await sleep(1000);
      // (Typed keys have been lost now and then: the launcher still shows its Favorites list. A search shows results over the whole popup, so
      // the Favorites row's blue is gone: look at that pixel, and type again if it is still there.)
      const browsing = async () => { const px = await evaluate("(async () => { let u = rvDisplay.shot(); if (u && u.then) u = await u; const img = new Image(); img.src = u; await img.decode(); const c = document.createElement('canvas'); c.width = img.width; c.height = img.height; const g = c.getContext('2d'); g.drawImage(img, 0, 0); const d = g.getImageData(" + (pop.x + 100) + ", " + (pop.y + 69) + ", 1, 1).data; return [d[0], d[1], d[2]]; })()");
        return !!px && px[2] - px[0] > 12; };
      for (let attempt = 1; attempt <= 4; attempt++) {
        await typeText(p.search); await sleep(2500);
        if (!(await browsing())) { say("SEARCH typed " + JSON.stringify(p.search) + (attempt > 1 ? " (attempt " + attempt + ")" : "")); break; }
        say("SEARCH typed " + JSON.stringify(p.search) + " but the launcher shows no results (attempt " + attempt + ")");
        await click(pop.x + 420, pop.y + 23); await sleep(800);
      }
      if (shotDir) { const r = await Promise.race([send("Page.captureScreenshot", { format: "png" }, sessionId), sleep(10000)]).catch(() => null); if (r) fs.writeFileSync(path.join(shotDir, name + "-search.png"), Buffer.from(r.data, "base64")); }
      await click(pop.x + 78, pop.y + 105);    // the first result
    };
    let first = true, barShotsTaken = 0, libsShot = false;
    for (const app of apps) {
      const before = errors.length;
      const p = PROGRAMS[app] || { search: app, id: app };
      const have = (await displayWindows()).filter((w) => w.kind === 1 && (w.appId || "").toLowerCase().includes(p.id)).length;
      const site0 = await siteStats(); const rss0 = Math.round(rss());
      const dlAt = await evaluate("window.linuxOs && window.linuxOs.dl ? window.linuxOs.dl.log.length : 0") || 0;
      const t1 = Number(T());
      const pn0 = args.includes("--probe") ? Number(await evaluate("performance.now()")) : 0;
      say("EPOCH " + (Date.now() / 1000).toFixed(1) + " LAUNCH " + app + " via " + via + " (its windows before: " + have + ", rss " + rss0 + " MB)");
      if (via === "terminal") await evaluate("window.linuxOs.key_input(" + JSON.stringify("debian 'desktop wayland nqs-run " + app + "' > /tmp/" + app + ".out 2>&1 &\r") + "), 1");
      else await startPanel(app);
      const tClick = Number(T());
      let up = null, winInfo = null, lastNote = 0, hid = false, noteSeen = null, noteAt = 0;
      while (Number(T()) - tClick < budget * 2) {
        await pump();
        const wins = await displayWindows();
        const mine = wins.filter((w) => w.kind === 1 && (w.appId || "").toLowerCase().includes(p.id));
        if (mine.length > have) { up = Number(T()) - tClick; winInfo = mine[mine.length - 1].appId + ":" + mine[mine.length - 1].title; break; }
        const n = await evaluate("(() => { const el = document.getElementById('launchnote'); return el && !el.hidden ? el.textContent.trim().slice(0, 140) : ''; })()");
        if (n && barShots && !libsShot && /libraries/.test(n)) { libsShot = true; await barShot("bar-starting-" + app + "-libs"); }
        if (n && barShots && barShotsTaken < 2 && (barShotsTaken === 0 || Number(T()) - noteAt >= 15)) { barShotsTaken++; await barShot("bar-starting-" + app + "-" + barShotsTaken); }
        if (n) { if (!noteSeen || Number(T()) - noteAt >= 20) { noteAt = Number(T()); say("page note: " + n.slice(0, 120)); } noteSeen = n; }
        if (Number(T()) - lastNote >= 20) { lastNote = Number(T()); say("waiting for " + app + ": " + Math.round(Number(T()) - tClick) + " s, rss " + sampleRss() + " MB"); }
        if (first && hideAfter && !hid && Number(T()) - tClick >= hideAfter) {
          hid = true; say("HIDING the page (another tab in front) for 90 s");
          const other = await send("Target.createTarget", { url: "about:blank", background: false });
          await send("Target.activateTarget", { targetId: other.targetId });
          const hiddenNow = await send("Runtime.evaluate", { expression: "document.hidden", returnByValue: true }, sessionId).then((r) => r.result.value).catch(() => "?");
          say("document.hidden = " + hiddenNow);
          for (let i = 0; i < 90; i += 5) { await sleep(5000); await pump(); }
          await send("Target.activateTarget", { targetId });
          say("the page is in front again");
        }
        await sleep(500);
      }
      first = false;
      say("EPOCH " + (Date.now() / 1000).toFixed(1) + " WINDOW " + app);
      if (barShots) {
        // the page's tick sees the window within a second; the message stays a few seconds
        let done = "";
        for (let i = 0; i < 12 && !/Launched/.test(done); i++) { done = await evaluate("(() => { const el = document.querySelector('#launchnote .ln-done'); return el && !el.hidden ? el.textContent : ''; })()") || ""; if (!/Launched/.test(done)) await sleep(250); }
        summary.apps[app] = { launched_note: done };
        await barShot("bar-launched-" + app);
        barShotsTaken = 0;
      }
      const site1 = await siteStats();
      const delta = site0 && site1 ? Object.fromEntries(Object.keys(site1).map((k) => [k, Math.round(((site1[k] || 0) - (site0[k] || 0)) / 1048576 * 10) / 10])) : null;
      sampleRss();
      const dl = await evaluate("(() => { const l = window.linuxOs && window.linuxOs.dl ? window.linuxOs.dl.log.slice(" + dlAt + ") : []; const w = l.filter((e) => e.runner !== 'main'); const c = w.filter((e) => e.how === 'compile'); return JSON.stringify({ worker_loads: w.length, worker_compiled: c.length, compile_ms: Math.round(c.reduce((n, e) => n + (e.us || 0), 0) / 1000), compiled_mib: Math.round(c.reduce((n, e) => n + (e.size || 0), 0) / 1048576), shared: w.filter((e) => e.how === 'shared').length, bg_main: l.filter((e) => e.runner === 'main').length }); })()");
      if (args.includes("--probe")) {
        const dump = await evaluate("JSON.stringify({ now: performance.now(), tasks: (globalThis.__rvt || []).filter((e) => e.t >= " + (pn0 - 100) + "), dl: (window.linuxOs.dl.log || []).filter((e) => e.t >= " + (pn0 - 100) + ").map((e) => [Math.round(e.t), e.how, e.size, e.us, e.runner]) })");
        fs.mkdirSync("build/ux/rvt", { recursive: true }); fs.writeFileSync("build/ux/rvt/" + (process.env.UX_TAG || "x") + "-" + app + "-" + Math.round(pn0) + ".json", JSON.stringify({ pn0, up_s: up, dump: JSON.parse(dump) }));
      }
      summary.apps[app] = { ...summary.apps[app], dl: dl ? JSON.parse(dl) : null, window_after_s: up === null ? null : Math.round(up * 10) / 10, window: winInfo, within_budget: up !== null && up <= budget, new_errors: errors.length - before, mib_served_during: delta, rss_mb: Math.round(rss()), page_note: noteSeen };
      say(app + ": " + JSON.stringify(summary.apps[app]));
      if (shotDir) { const r = await Promise.race([send("Page.captureScreenshot", { format: "png" }, sessionId), sleep(15000)]).catch(() => null); if (r) fs.writeFileSync(path.join(shotDir, app + ".png"), Buffer.from(r.data, "base64")); }
      if (up === null) break;   // (a program that did not come up: the rest would only pile on)
      await sleep(6000); await pump();
      await press("Escape");
    }
    if (args.includes("--bar-stack") && shotDir) {
      // (page side only: the launch channel is the one the window server tells the page on)
      const post = (m) => evaluate("(() => { const c = window.rvChannel('rv-plasma'); c.postMessage(" + JSON.stringify(m) + "); c.close(); return 1; })()");
      for (const id of ["org.kde.kcalc", "org.kde.kwrite", "org.kde.okular"]) await post({ type: "launch", appId: id });
      await sleep(9000); await barShot("bar-stack-starting");
      await post({ type: "launch-done", appId: "org.kde.kwrite" }); await sleep(1500); await barShot("bar-stack-one-done");
      await post({ type: "launch-done", appId: "org.kde.kcalc" }); await post({ type: "launch-done", appId: "org.kde.okular" }); await sleep(1500); await barShot("bar-stack-all-done");
      await sleep(1700); await barShot("bar-stack-fading");
      await sleep(2500); summary.bar_stack_after = await barShot("bar-stack-gone");
    }
    if (controlFile) {
      fs.writeFileSync(controlFile + ".out", ""); if (!fs.existsSync(controlFile)) fs.writeFileSync(controlFile, "");
      const out = (t) => fs.appendFileSync(controlFile + ".out", "T+" + T() + " " + t + "\n");
      const rightClick = async (x, y) => { const p = await toClient(x, y);
        await send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y }, sessionId); await sleep(150);
        await send("Input.dispatchMouseEvent", { type: "mousePressed", x: p.x, y: p.y, button: "right", buttons: 2, clickCount: 1 }, sessionId); await sleep(80);
        await send("Input.dispatchMouseEvent", { type: "mouseReleased", x: p.x, y: p.y, button: "right", buttons: 0, clickCount: 1 }, sessionId); };
      let done = 0, quit = false; say("CONTROL: reading " + controlFile);
      while (!quit && Number(T()) < maxSeconds - 60) {
        await pump();
        const cmds = fs.readFileSync(controlFile, "utf8").split("\n").filter((l) => l.trim());
        for (; done < cmds.length && !quit; done++) {
          const line = cmds[done], [c, ...a] = line.trim().split(/\s+/); const rest = line.trim().slice(c.length).trim();
          try {
            if (c === "click") { await click(Number(a[0]), Number(a[1])); out("ok " + line); }
            else if (c === "dclick") { await click(Number(a[0]), Number(a[1])); await sleep(120); await click(Number(a[0]), Number(a[1])); out("ok " + line); }
            else if (c === "rclick") { await rightClick(Number(a[0]), Number(a[1])); out("ok " + line); }
            else if (c === "wheel") { const p = await toClient(Number(a[0]), Number(a[1])); await send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y }, sessionId); await send("Input.dispatchMouseEvent", { type: "mouseWheel", x: p.x, y: p.y, deltaX: 0, deltaY: Number(a[2]) }, sessionId); out("ok " + line); }
            else if (c === "cclick") { await key("rawKeyDown", "ControlLeft"); await sleep(200); await click(Number(a[0]), Number(a[1])); await sleep(200); await key("keyUp", "ControlLeft"); out("ok " + line); }   // (Ctrl held: a link in a terminal)
            else if (c === "pclick") { const r = JSON.parse(await evaluate("JSON.stringify((() => { const e = document.querySelector(" + JSON.stringify(rest) + "); if (!e) return null; const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })())") || "null"); if (!r) { out("error pclick: no element " + rest); } else { for (const [type, buttons] of [["mouseMoved", 0], ["mousePressed", 1], ["mouseReleased", 0]]) { await send("Input.dispatchMouseEvent", { type, x: r.x, y: r.y, button: "left", buttons, clickCount: 1 }, sessionId); await sleep(80); } out("ok " + line); } }   // (a DOM element of the page itself: the host-open prompt's buttons)
            else if (c === "pkey") { const vk = { Enter: 13, Escape: 27, Tab: 9 }[a[0]]; await send("Input.dispatchKeyEvent", { type: "rawKeyDown", key: a[0], code: a[0], windowsVirtualKeyCode: vk }, sessionId); await sleep(40); await send("Input.dispatchKeyEvent", { type: "keyUp", key: a[0], code: a[0], windowsVirtualKeyCode: vk }, sessionId); out("ok " + line); }   // (to what has the page's focus, not the screen)
            else if (c === "targets") { out(JSON.stringify((await send("Target.getTargets")).targetInfos.filter((t) => t.type === "page").map((t) => t.url))); }
            else if (c === "type") { await typeText(rest); out("ok " + line); }
            else if (c === "key") { await press(a[0]); out("ok " + line); }
            else if (c === "wait") { for (let i = 0; i < Number(a[0]); i++) { await sleep(1000); if (i % 5 === 0) await pump(); } out("ok " + line); }
            else if (c === "shot") { const r = await Promise.race([send("Page.captureScreenshot", { format: "png" }, sessionId), sleep(15000)]).catch(() => null); if (r && shotDir) { fs.writeFileSync(path.join(shotDir, a[0] + ".png"), Buffer.from(r.data, "base64")); out("ok shot " + path.join(shotDir, a[0] + ".png")); } else out("shot failed"); }
            else if (c === "wins") out(JSON.stringify(await displayWindows()));
            else if (c === "js") out(String(await evaluate(rest)));
            else if (c === "quit") { quit = true; out("ok quit"); }
            else out("unknown command: " + line);
          } catch (e) { out("error " + line + ": " + e.message); }
        }
        await sleep(400);
      }
    }
    // everything that came up is still there after a while, the window server and the display are alive
    await sleep(dwell * 1000); await pump();
    const wins = await displayWindows();
    const alive = await evaluate("(() => { const d = window.rvDisplay; return d && d.active ? d.active() : null })()");
    const stopped = await evaluate("(() => { const m = [...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent).join('\\n'); return /window server stopped/.test(m) })()");
    const up = Object.entries(summary.apps).filter(([, v]) => v.window_after_s !== null).map(([k]) => k);
    summary.still_there = up.every((a) => wins.some((w) => w.kind === 1 && (w.appId || "").toLowerCase().includes((PROGRAMS[a] || { id: a }).id)));
    summary.display_active = alive; summary.window_server_stopped_text = stopped;
    summary.site_bytes = await siteStats();
    
  }
  await pump();
  summary.errors_total = errors.length; summary.log_error_lines = lines.length; summary.peak_rss_mb = Math.max(peakRss, Math.round(rss()));
  summary.notes = notes.slice(-12); summary.rss_samples = samples.filter((_, i) => i % 3 === 0).slice(-60);
  const all = apps.every((a) => summary.apps[a] && summary.apps[a].within_budget);
  summary.gate = ready !== null && all && errors.length === 0 && lines.length === 0 && summary.still_there === true && summary.display_active !== false && !summary.window_server_stopped_text ? "PASS" : "FAIL";
  say("GATE " + summary.gate + " (budget " + budget + " s per window)");
  console.error(JSON.stringify(summary));
  await send("Browser.close").catch(() => {});
  await new Promise((r) => { chrome.on("exit", r); setTimeout(() => { chrome.kill("SIGKILL"); r(); }, 15000); });
  process.exit(summary.gate === "PASS" ? 0 : 1);
})().catch((e) => { console.error(e); chrome.kill("SIGKILL"); process.exit(1); });
