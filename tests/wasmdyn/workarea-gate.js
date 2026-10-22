#!/usr/bin/env node
// Gate for the usable area (work area): where maximized windows go when panels are not at the bottom, and what fullscreen covers.
// Drives tests/wasmdyn/native-chrome.js through its --control file (so the browser, profile, launch and Worker-error accounting are
// that script's), and has to run under tools/with-browser.sh like it:
//
//   RV_BROWSER_WHO=who tools/with-browser.sh WHO node tests/wasmdyn/workarea-gate.js --url http://127.0.0.1:PORT/dev.html?disk=release \
//        --site http://127.0.0.1:PORT --profile DIR --shot DIR [--apps systemsettings,dolphin,konsole] [--window 1280x900] [--expect top|bottom] [--theme]
//
// --theme: first (System Settings is the first program, at its default place, 1280x900 window) Colors & Themes > Global Theme > Get New >
// Catppuccin Mocha > Install, then the tile with "Desktop and window layout" ticked > Apply: the panel moves to the top (the clicks are the
// screen's pixels of that window size; pictures of each step in DIR/theme-N.png). The other programs are started after it.
//
// For each program (started by the terminal route, `debian 'desktop wayland nqs-run APP'`, which does not depend on the panel layout):
//   - it is maximized (the page's order, as the title bar's button sends it): the window's frame (its title bar included) lies
//     within the usable area the page reads from the server (rvDisplay.workArea()), its content ends where the area does, and the
//     frame does not touch any panel's visible part (a layer surface's geometry in the table), so the title bar is fully below a top panel;
//   - with --expect top|bottom, a panel is on that edge (a layer surface of the dock role, or the page's own panel strip);
//   - F11: the window is fullscreen (flags), its content is the whole screen, and it is above every panel; F11 again brings the maximized
//     window back.
// Pictures: DIR/APP-maximized.png, DIR/APP-fullscreen.png. Prints "GATE PASS|FAIL" and a JSON line; exit 0 on PASS.
"use strict";
const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");
const args = process.argv.slice(2);
const opt = (n, d) => (args.includes("--" + n) ? args[args.indexOf("--" + n) + 1] : d);
const url = opt("url"), site = opt("site"), profile = opt("profile"), shot = opt("shot");
const apps = opt("apps", "systemsettings,dolphin,konsole").split(",");
const windowSize = opt("window", "1280x900"), expect = opt("expect", ""), controlMax = opt("control-max", "2400");
const theme = args.includes("--theme");   // install + apply Catppuccin Mocha (with its desktop layout) through System Settings first (needs the relay)
const who = process.env.RV_BROWSER_WHO || "workarea-gate";
if (!url || !shot) { console.error("usage: see the header of this file"); process.exit(2); }
fs.mkdirSync(shot, { recursive: true });
const control = path.join(shot, "control"), out = control + ".out";
fs.writeFileSync(control, ""); fs.writeFileSync(out, "");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const here = __dirname;
const child = spawn("node", [path.join(here, "native-chrome.js"), "--url", url, "--apps", theme ? apps[0] : apps.join(","), "--via", "terminal", "--window", windowSize,
  "--shot", shot, "--control", control, "--control-max", controlMax, "--ready-timeout", opt("ready-timeout", "900"), "--budget", opt("budget", "180"),
  ...(site ? ["--site", site] : []), ...(profile ? ["--profile", profile] : [])], { stdio: ["ignore", "inherit", "pipe"], env: process.env });
let log = ""; child.stderr.on("data", (d) => { log += d; process.stderr.write(d); });
let exited = null; child.on("exit", (c) => { exited = c; });
const T = () => ((Date.now() - t0) / 1000).toFixed(1), t0 = Date.now();
const say = (s) => console.error("T+" + T() + " gate: " + s);
// One command to the control file; its answer (the text after the "T+s " stamp) is the next line of FILE.out that is new.
let sent = 0, answered = 0;
const ask = async (cmd, timeoutS = 60) => {
  fs.appendFileSync(control, cmd + "\n"); sent++;
  const t = Date.now();
  while (Date.now() - t < timeoutS * 1000 && exited === null) {
    const lines = fs.readFileSync(out, "utf8").split("\n").filter(Boolean);
    if (lines.length > answered) { const l = lines[answered++]; return l.replace(/^T\+[\d.]+ /, ""); }
    await sleep(300);
  }
  throw new Error("no answer to: " + cmd);
};
const js = async (expr) => { const a = await ask("js " + expr); try { return JSON.parse(a); } catch (e) { return a; } };
const SNAP = "JSON.stringify((() => { const d = window.rvDisplay, s = d.screenSize(), w = d.windows(); return { screen: s, work: d.workArea(), title: d.titleHeight ? d.titleHeight() : 28," +
  " native: d.nativeShell(), wins: w.map((x) => ({ id: x.id, kind: x.kind, layer: x.layer, z: x.z, x: x.x, y: x.y, w: x.w, h: x.h, g: x.geometry, flags: x.flags, appId: x.appId, title: x.title })) }; })())";
const F_MAX = 4, F_FULL = 8, F_DEC = 32, F_MIN = 64;
const rectsOverlap = (a, b) => a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
const results = { apps: {}, checks: [], pass: true };
const check = (name, ok, detail) => { results.checks.push({ name, ok: !!ok, detail }); if (!ok) results.pass = false; say((ok ? "ok   " : "FAIL ") + name + (detail ? " " + JSON.stringify(detail) : "")); };
const snap = async () => js(SNAP);
const panels = (s) => s.wins.filter((w) => w.kind === 6 && w.layer >= 1).map((w) => ({ id: w.id, z: w.z, rect: [w.x + (w.g ? w.g[0] : 0), w.y + (w.g ? w.g[1] : 0), w.g && w.g[2] > 0 ? w.g[2] : w.w, w.g && w.g[3] > 0 ? w.g[3] : w.h] }));
const find = (s, app) => s.wins.filter((w) => w.kind === 1 && (w.appId || "").toLowerCase().includes(app)).pop();
const shotTo = async (name) => { const r = await ask("shot " + name, 30); return r; };

(async () => {
  // the control loop starts when the programs are up (native-chrome.js reads FILE after its launches)
  const t = Date.now();
  while (!/CONTROL: reading/.test(log) && exited === null && Date.now() - t < 3000 * 1000) await sleep(2000);
  if (exited !== null) { console.error("native-chrome.js ended before the control loop: " + exited); process.exit(1); }
  say("programs are up");
  let s = await snap();
  results.screen = s.screen; results.native = s.native;
  const edgeOf = (r, sh) => (r[1] <= 1 && r[3] < sh / 3 ? "top" : r[1] + r[3] >= sh - 1 && r[3] < sh / 3 ? "bottom" : "other");
  const panelEdges = (s) => { const ps = panels(s); return ps.length ? ps.map((p) => edgeOf(p.rect, s.screen[1])) : [s.work[1] > 0 ? "top" : s.work[1] + s.work[3] < s.screen[1] ? "bottom" : "none"]; };
  // one program: maximized, usable area, title bar clear of the panels' visible parts, fullscreen
  const phase = async (label, app) => {
    s = await snap();
    let w = find(s, app);
    const name = label + " " + app;
    if (!w) { check(name + " has a window", false); return; }
    results.apps[name] = {};
    // (the program is brought to the front first: the earlier ones' windows may lie over it)
    await js("rvDisplay.order(1, " + w.id + ")"); await sleep(1500);
    // maximize (a window that starts maximized is restored first, so that the order is what is tested)
    if (w.flags & F_MAX) { await js("rvDisplay.order(7, " + w.id + ")"); await sleep(2500); }
    await js("rvDisplay.order(7, " + w.id + ")");
    let ok = false;
    for (let i = 0; i < 20 && !ok; i++) { await sleep(700); s = await snap(); w = find(s, app); ok = !!w && (w.flags & F_MAX) !== 0; }
    check(name + " maximized", ok);
    await sleep(2500); s = await snap(); w = find(s, app);
    const [ax, ay, aw, ah] = s.work, title = (w.flags & F_DEC) ? s.title : 0;
    const frame = [w.x, w.y - title, w.w, w.h + title], bar = [w.x, w.y - title, w.w, title];
    results.apps[name].maximized = { frame, work: s.work, panels: panels(s).map((p) => p.rect) };
    check(name + " frame within the usable area", frame[0] >= ax - 1 && frame[1] >= ay - 1 && frame[0] + frame[2] <= ax + aw + 1 && frame[1] + frame[3] <= ay + ah + 1, { frame, work: s.work });
    check(name + " fills the usable area", Math.abs(frame[1] - ay) <= 1 && Math.abs(frame[1] + frame[3] - (ay + ah)) <= 1 && Math.abs(frame[0] - ax) <= 1 && Math.abs(frame[2] - aw) <= 1, { frame, work: s.work });
    // (the title bar: a floating dock without an exclusive zone may lie over a maximized window's lower edge, as in Plasma)
    const hit = panels(s).filter((p) => p.z > w.z && rectsOverlap(bar, p.rect));
    check(name + " title bar clear of the panels' visible parts", hit.length === 0, { bar, panels: hit.map((p) => p.rect) });
    await shotTo(label + "-" + app + "-maximized");
    // F11 (the window is activated first: it has the keyboard, as a person who maximized it has it)
    await js("rvDisplay.order(1, " + w.id + ")"); await sleep(2000);
    s = await snap(); w = find(s, app); say(name + " before F11: flags " + w.flags + " (2 = active), top window of the stack: " + JSON.stringify(s.wins.filter((x) => x.kind === 1).sort((a, b) => b.z - a.z).slice(0, 2).map((x) => [x.appId, x.z, x.flags])));
    await ask("key F11"); await sleep(2500);
    s = await snap(); w = find(s, app);
    let full = !!w && (w.flags & F_FULL) !== 0;
    if (!full) { say(name + " ignored F11; the title bar's fullscreen order instead"); await js("rvDisplay.order(9, " + w.id + ")"); await sleep(2500); s = await snap(); w = find(s, app); full = !!w && (w.flags & F_FULL) !== 0; results.apps[name].f11_ignored = true; }
    check(name + " fullscreen", full);
    if (full) {
      check(name + " fullscreen covers the whole screen", w.x === 0 && w.y === 0 && Math.abs(w.w - s.screen[0]) <= 1 && Math.abs(w.h - s.screen[1]) <= 1, { rect: [w.x, w.y, w.w, w.h], screen: s.screen });
      const above = panels(s).filter((p) => p.z > w.z);
      check(name + " fullscreen is above every panel", above.length === 0, { panels_above: above.length });
      await shotTo(label + "-" + app + "-fullscreen");
      if (results.apps[name].f11_ignored) await js("rvDisplay.order(9, " + w.id + ")"); else await ask("key F11");
      await sleep(3000); s = await snap(); w = find(s, app);
      check(name + " leaves fullscreen", !!w && (w.flags & F_FULL) === 0 && (w.flags & F_MAX) !== 0, { flags: w && w.flags });
    }
    // back to a normal window for the next program
    s = await snap(); w = find(s, app);
    if (w && (w.flags & F_MAX)) { await js("rvDisplay.order(7, " + w.id + ")"); await sleep(1500); }
  };
  say("screen " + s.screen.join("x") + ", work area " + s.work.join(",") + ", title " + s.title + ", panels " + JSON.stringify(panels(s).map((p) => p.rect)) + ", plasmashell shell: " + s.native);
  if (!theme) {
    if (expect) check("a panel is on the " + expect + " edge", panelEdges(s).includes(expect), { panels: panelEdges(s), work: s.work });
    for (const app of apps) await phase("session", app);
  } else {
    // 1. the default session (the panel at the bottom), 2. Catppuccin Mocha with its layout (a bar at the top, a dock at the bottom)
    check("default: a panel is at the bottom", panelEdges(s).includes("bottom") && !panelEdges(s).includes("top"), { panels: panelEdges(s), work: s.work });
    await phase("default", apps[0]);
    const steps = [["click 122 259", 4], ["click 907 53", 9], ["click 742 92", 1], ["type catppuccin mocha", 1], ["key Enter", 12], ["click 330 200", 6], ["click 800 92", 45],
                   ["click 830 58", 4], ["click 412 310", 4], ["click 381 377", 2], ["click 760 447", 30]];
    let n = 0;
    for (const [cmd, wait] of steps) {
      await ask(cmd); await sleep(wait * 1000);
      if (cmd.startsWith("click 8") || cmd.startsWith("key") || cmd.startsWith("click 4") || cmd.startsWith("click 3")) { await ask("shot theme-" + (++n)); }
    }
    let top = false;
    for (let i = 0; i < 40 && !top; i++) { s = await snap(); top = panels(s).some((p) => p.rect[1] <= 1 && p.rect[3] < s.screen[1] / 3 && p.rect[2] > s.screen[0] / 2); if (!top) await sleep(3000); }
    check("the theme's layout put a panel at the top", top, { panels: panels(s).map((p) => p.rect), work: s.work });
    await sleep(5000);
    for (const app of apps.slice(1)) {
      await ask("js window.linuxOs.key_input(" + JSON.stringify("debian 'desktop wayland nqs-run " + app + "' > /tmp/" + app + ".out 2>&1 &\r") + ", 1)");
      for (let i = 0; i < 90; i++) { await sleep(2000); s = await snap(); if (find(s, app)) break; }
      check(app + " started", !!find(s, app));
    }
    await sleep(3000);
    s = await snap();
    say("catppuccin: work area " + s.work.join(",") + ", panels " + JSON.stringify(panels(s).map((p) => p.rect)));
    check("catppuccin: bar at the top, dock at the bottom", panelEdges(s).includes("top"), { panels: panelEdges(s), work: s.work });
    for (const app of apps) await phase("catppuccin", app);
  }
  // the window server's own account of the panels (its log), for the record
  await js("window.linuxOs.key_input(\"clear; debian 'grep -a -h layershell /root/wlserver.log | cut -c1-200 | tail -30'\\r\", 1)").catch(() => {});
  await sleep(10000);
  const logRows = await js("[...document.querySelectorAll('.xterm-rows > div')].map((d) => d.textContent.trimEnd()).filter(Boolean).slice(-32).join(' || ')").catch(() => "");
  results.wlserver_log = String(logRows).split(" || ");
  say("wlserver log: " + String(logRows).replace(/ \|\| /g, "\n  "));
  await ask("quit", 30).catch(() => {});
  const t2 = Date.now(); while (exited === null && Date.now() - t2 < 120000) await sleep(1000);
  const errorLines = (log.match(/EXCEPTION \[/g) || []).length;
  const gateLine = /GATE (PASS|FAIL)/.exec(log);
  check("0 Worker/page errors", errorLines === 0 && !/THE TAB CRASHED/.test(log), { exceptions: errorLines, native_chrome_gate: gateLine && gateLine[1] });
  console.error(JSON.stringify(results));
  console.error("GATE " + (results.pass ? "PASS" : "FAIL"));
  process.exit(results.pass ? 0 : 1);
})().catch((e) => { console.error(e); try { child.kill("SIGTERM"); } catch (x) {} process.exit(1); });
