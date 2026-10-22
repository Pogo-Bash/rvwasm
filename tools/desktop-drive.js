#!/usr/bin/env node
// Use the machine's desktop from a script: start something on Linux/Wasm,
// then move the pointer, click, type and take pictures of the screen, as a
// person at the page would (the events are the browser's own, sent to the
// page's canvas through the DevTools protocol).
//
//   node tools/desktop-drive.js [--disk NAME] [--image NAME] [--pool N] [--query A=B] [--headed] 'COMMAND' ACTION...
//
// COMMAND is typed into the machine's terminal and not waited for (it is the
// desktop, and stays): e.g. 'debian desktop plasma'. Then the actions, in order:
//
//   wait:SECONDS          do nothing for a while
//   shot:FILE.png         the screen as it is now (with the window server's windows, when it runs)
//   move:X,Y              the pointer, in the screen's pixels
//   click:X,Y             left button; rclick:X,Y the right one, mclick:X,Y the middle one (paste the
//                         primary selection), dclick:X,Y twice, tclick:X,Y three times (a line in a terminal)
//   drag:X,Y,X2,Y2        press at one place, let go at the other
//   press:X,Y, moveto:X,Y[,STEPS], release:X,Y   the same in parts (a person's drag: press, a pause, move in
//                         steps of 50 ms, let go), so that pictures can be taken in the middle
//   pressel:SELECTOR      press: at the middle of the page's element that matches SELECTOR
//   type:TEXT             the keys that make TEXT on a US keyboard
//   mdrag:X,Y,X2,Y2       the middle button held from one point to the other (Blender orbits its view so)
//   fkey:NAME             as key:, but to whatever has the keyboard (a page shown in a window), not the screen
//   key:NAME              one key by the browser's name for it (Enter, Escape, Tab, F2, ArrowDown, ...),
//                         with modifiers before it: key:ctrl+alt+KeyT, key:alt+F4, key:meta
//   sh:COMMAND            a second command on the terminal, waited for, its output printed
//                         (only if COMMAND above ended or was put in the background with &)
//   log:N                 the last N lines of the page's log
//   workers               how many Workers (the page's and those they started) are alive now (--worker-errors)
//   kernel                what the kernel's own variables say (clock, run queues, disk requests), read from memory
//   js:EXPRESSION         the value of a JavaScript expression evaluated in the page (after eval:tools/kde-perf-probe.js:
//                         js:kdePerf.rate(5) and the like)
//   eval:FILE.js          the value of the JavaScript expression in FILE, evaluated in the page (tools/kernel-stacks.js:
//                         the tasks' kernel stacks, read from memory)
//   win:MATCH             coordinates from here on are those of the window server's window whose title or app id
//                         has MATCH in it (the topmost), from the top left of its contents; win: alone, the screen's
//   pageshot:FILE.png     the page as the browser shows it, cut to the screen (with what the page draws over it in
//                         the DOM: the window server's frames)
//   clickel:SELECTOR      a click in the middle of the page's element that matches SELECTOR (the page's own DOM:
//                         the bar, a prompt)
//   tabs                  the browser's tabs now, by address (what the page opened)
//   hostclip:TEXT         put TEXT on the browser's clipboard (navigator.clipboard.writeText), as another tab would
//   readclip              print what is on the browser's clipboard (navigator.clipboard.readText)
//   viewport:W,H[,DPR]    make the page's window W x H CSS pixels at device pixel ratio DPR (default 1)
//   keydown:NAME, keyup:NAME   one key pressed or let go (to hold Alt while Tab is pressed, say)
//   until:S:EXPRESSION    wait (at most S seconds) until the page's EXPRESSION is true, and say how long that took
//   wheel:X,Y,DY          the mouse wheel over X,Y (DY > 0 down; 100 a notch)
//   insert:TEXT           TEXT committed as an input method does (Input.insertText): CJK and the like
//   compose:TEXT          TEXT composed by an input method (composition events), then committed
//   repl:DIR              actions from files DIR/in-N.txt as they come, output to DIR/out-N.txt (see repl())
//   hostdrop:FILE[,X,Y]   drop FILE (on this computer) on the screen at X,Y as a person would drag it from the
//                         desktop (the browser's own drag events), and wait until the page says it went in
// The page may use the clipboard without asking (the driver grants it), and counts as focused throughout.
// --on-hang FILE.js: evaluate that as well when an sh: action does not end (a machine that has panicked can still be read).
//   mark                  note the time, for the waits below
//   waitwin:REGEX[,S]     wait (at most S seconds, default 600) for a window of the window server whose title or app
//                         id matches REGEX, and say how long after the last mark it came, and what it is
//   waittitle:REGEX[,S]   the same for a window whose title matches REGEX (a page's title in a browser's window)
// --worker-errors: every Worker of the page (and the Workers they start) is attached to as it starts, and an exception
// thrown in one is printed ("---- a Worker threw: ...") and counted; "worker errors: N" at the end, exit status 6 when N > 0.
// (Not with --profile, which attaches to the Workers as well and does not count.)
// --profile US: every Worker of the page is attached to as it starts, with the browser's profiler ready in it
// (sampling every US microseconds); then
//   profstart             start the profiler in every Worker there is (and in those that start later)
//   profstop:FILE.json    stop it and write, by Worker, the samples by function: its own (self) and those
//                         with it anywhere in the stack (incl); tools/chromeprof.py makes sense of it
//
// Needs what tools/bench-browser.js needs: the dev server, Chrome for Testing in tools/chrome.

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
const option = (name) => {
  const at = args.indexOf(name);
  if (at < 0) {
    return null;
  }
  return args.splice(at, 2)[1];
};
const flag = (name) => {
  const at = args.indexOf(name);
  if (at >= 0) {
    args.splice(at, 1);
  }
  return at >= 0;
};
const disk = option("--disk"), image = option("--image"), pool = option("--pool");
const base = option("--url") || "http://localhost:8000";
const query_extra = option("--query");  // more for the page's query string, as given (a=b&c=d)
const headed = flag("--headed");
const sh_timeout = Number(option("--sh-timeout")) || 180000;
const on_hang = option("--on-hang");
const profile_us = Number(option("--profile")) || 0;
const worker_errors = flag("--worker-errors");
if (args.length < 1) {
  console.error("usage: desktop-drive.js [--disk NAME] [--image NAME] [--pool N] [--headed] 'COMMAND' ACTION...");
  process.exit(2);
}
const CHROME = process.env.CHROME || path.join(__dirname,
  "chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
// --sh-timeout MS: how long an sh: action may take (default 180000). When one does not end in time:
const DIAGNOSE = "cat /proc/loadavg; grep -E '^nr_file_pages|^nr_dirty |^nr_writeback |^nr_free_pages|^drop_pagecache' "
  + "/proc/vmstat | tr '\\n' ' '; echo; ps | grep -v ' \\[' | awk '{ print $1, $4, $5, $6, $7, $8 }' | head -70; dmesg | tail -12";
let hung = false;

// What to press for a character: the key's name, and whether Shift is down.
const KEYS = {};
for (const c of "abcdefghijklmnopqrstuvwxyz") {
  KEYS[c] = ["Key" + c.toUpperCase(), false];
  KEYS[c.toUpperCase()] = ["Key" + c.toUpperCase(), true];
}
"1234567890".split("").forEach((c, i) => {
  KEYS[c] = ["Digit" + c, false];
  KEYS["!@#$%^&*()"[i]] = ["Digit" + c, true];
});
for (const [plain, shifted, name] of [["-", "_", "Minus"], ["=", "+", "Equal"], ["[", "{", "BracketLeft"],
     ["]", "}", "BracketRight"], [";", ":", "Semicolon"], ["'", "\"", "Quote"], ["`", "~", "Backquote"],
     ["\\", "|", "Backslash"], [",", "<", "Comma"], [".", ">", "Period"], ["/", "?", "Slash"]]) {
  KEYS[plain] = [name, false];
  KEYS[shifted] = [name, true];
}
KEYS[" "] = ["Space", false];
KEYS["\n"] = ["Enter", false];
KEYS["\t"] = ["Tab", false];
const MODIFIERS = { ctrl: "ControlLeft", alt: "AltLeft", shift: "ShiftLeft", meta: "MetaLeft" };

// repl:DIR  actions from files, for a person (or an agent) at the desktop between its own turns: the driver
// waits for DIR/in-N.txt (N = 0, 1, ...; write it elsewhere and rename it in), runs its lines as actions,
// and writes what they printed to DIR/out-N.txt (renamed in when complete). A line "quit" ends it, and so do
// REPL_IDLE seconds without a file (default 600) or REPL_MAX seconds in all (default 1800): the browser is
// shared, a forgotten session must not keep it.
async function repl(dir, act) {
  fs.mkdirSync(dir, { recursive: true });
  const idle = (Number(process.env.REPL_IDLE) || 600) * 1000, max = (Number(process.env.REPL_MAX) || 1800) * 1000;
  const start = Date.now();
  let last = Date.now();
  console.log("repl: waiting for " + path.join(dir, "in-0.txt"));
  for (let n = 0; ; ) {
    if (Date.now() - start > max || Date.now() - last > idle) {
      console.log("repl: time is up");
      return;
    }
    const file = path.join(dir, "in-" + n + ".txt");
    if (!fs.existsSync(file)) {
      await sleep(250);
      continue;
    }
    last = Date.now();
    const lines = fs.readFileSync(file, "utf8").split("\n").filter((l) => l.trim() && !l.startsWith("#"));
    const out = [];
    const log = console.log;
    console.log = (...parts) => {
      out.push(parts.join(" "));
      log(...parts);
    };
    let quit = false;
    for (const line of lines) {
      if (line.trim() == "quit") {
        quit = true;
        break;
      }
      const t0 = Date.now();
      try {
        await act(line);
      } catch (error) {
        out.push("ERROR " + line + ": " + error.message);
      }
      out.push("  (" + (Date.now() - t0) + " ms) " + line.slice(0, 80));
    }
    console.log = log;
    fs.writeFileSync(path.join(dir, "out-" + n + ".tmp"), out.join("\n") + "\n");
    fs.renameSync(path.join(dir, "out-" + n + ".tmp"), path.join(dir, "out-" + n + ".txt"));
    n++;
    if (quit) {
      return;
    }
  }
}

async function main() {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "rvdrive-"));
  const chrome = spawn(CHROME, [
    ...(headed ? [] : ["--headless=new"]),
    "--user-data-dir=" + profile, "--remote-debugging-port=0", "--no-first-run", "--no-default-browser-check",
    "--window-size=1100,1500", "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
    "--disable-backgrounding-occluded-windows",
    // CHROME_FLAGS: more for the browser (e.g. --enable-blink-features=ForceEagerMeasureMemory, so that
    // performance.measureUserAgentSpecificMemory() answers at once instead of at the next collection).
    ...(process.env.CHROME_FLAGS ? process.env.CHROME_FLAGS.split(" ").filter(Boolean) : []),
    "about:blank",
  ], { stdio: ["ignore", "ignore", "pipe"] });
  // CHROME_LOG=FILE: what the browser says on its standard error (with CHROME_FLAGS="--enable-logging=stderr --v=0",
  // its log), kept in FILE, and how it ended; KEEP_PROFILE=1: its profile directory is left (crash reports in it,
  // Crashpad/), and named at the end.
  const chrome_log = process.env.CHROME_LOG ? fs.createWriteStream(process.env.CHROME_LOG) : null;
  if (chrome_log) {
    chrome.stderr.on("data", (data) => chrome_log.write(data));
  }
  // (Always said: a browser that went in the middle of a run was killed (signal), crashed, or quit (code).)
  chrome.on("exit", (code, signal) => {
    if (chrome_log) {
      chrome_log.write(`\n==== the browser ended: code ${code} signal ${signal} at ${new Date().toISOString()}\n`);
    }
    console.error(`the browser ended: code ${code} signal ${signal}`);
  });
  const address = await new Promise((resolve, reject) => {
    let err = "";
    chrome.stderr.on("data", (data) => {
      if (err.length > (1 << 20)) {
        return;
      }
      err += data;
      const m = err.match(/DevTools listening on (ws:\/\/\S+)/);
      if (m) {
        resolve(m[1]);
      }
    });
    chrome.on("exit", () => reject(new Error("Chrome exited: " + err)));
  });
  let status = 0;
  const event_hooks = [];
  try {
    const ws = new WebSocket(address);
    const waiting = new Map();
    let next = 1;
    ws.onmessage = (event) => {
      const msg = JSON.parse(event.data);
      if (msg.method == "Inspector.targetCrashed" || msg.method == "Target.targetCrashed") {
        console.error("the page crashed");
        chrome.kill();
        process.exit(3);
      }
      // A dialog (alert, confirm) stops the page's main thread, and with it the machine, until someone answers:
      // nobody is there to. Say what it said, and answer.
      if (msg.method == "Page.javascriptDialogOpening") {
        console.log("---- the page asked (" + msg.params.type + "): " + msg.params.message.replace(/\n+/g, " | "));
        ws.send(JSON.stringify({ id: next++, method: "Page.handleJavaScriptDialog", params: { accept: true },
                                 sessionId: msg.sessionId }));
      }
      if (msg.method == "Runtime.exceptionThrown") {
        const d = msg.params.exceptionDetails;
        console.log("---- the page threw: " + ((d.exception && d.exception.description) || d.text).split("\n").slice(0, 4).join(" | "));
      }
      for (const hook of event_hooks) {
        hook(msg);
      }
      if (msg.id && waiting.has(msg.id)) {
        const { resolve, reject } = waiting.get(msg.id);
        waiting.delete(msg.id);
        msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
      }
    };
    await new Promise((resolve, reject) => {
      ws.onopen = resolve;
      ws.onerror = () => reject(new Error("cannot connect to Chrome"));
    });
    // The connection does not survive the computer going to sleep, and nothing sent on a dead one is ever
    // answered: without this, a run that slept through its browser waits for good, holding the browser's lock.
    let done = false;
    ws.onclose = () => {
      if (!done) {
        console.error("the connection to the browser is gone (did the computer sleep?)");
        chrome.kill();
        if (process.env.KEEP_PROFILE) {
          console.error("the browser's profile is left in " + profile);
        }
        setTimeout(() => process.exit(5), 500);
      }
    };
    const send = (method, params = {}, sessionId) => {
      const id = next++;
      ws.send(JSON.stringify({ id, method, params, sessionId }));
      const answer = new Promise((resolve, reject) => waiting.set(id, { resolve, reject }));
      if (!method.startsWith("Input.")) {
        return answer;
      }
      // An input event is answered when the page's renderer has taken it: a renderer that is stuck (its main
      // thread busy) never does. Said, and gone on from, after 20 s, rather than waited for for good.
      return Promise.race([answer, sleep(20000).then(() => {
        if (waiting.has(id)) {
          waiting.delete(id);
          console.log("---- no answer in 20 s to " + method + " " + JSON.stringify(params).slice(0, 120) + " (a renderer is stuck?)");
        }
        return {};
      })]);
    };
    const { targetId } = await send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
    const evaluate = async (expression) => {
      const r = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId);
      if (r.exceptionDetails) {
        throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      }
      return r.result.value;
    };
    await send("Page.enable", {}, sessionId);
    await send("Runtime.enable", {}, sessionId);
    await send("Inspector.enable", {}, sessionId).catch(() => {});
    // The clipboard without a prompt, and a page that keeps its focus although no window of it is in front.
    await send("Browser.grantPermissions", { origin: base, permissions: ["clipboardReadWrite", "clipboardSanitizedWrite"] })
      .catch((error) => console.error("no clipboard permissions: " + error.message));
    await send("Emulation.setFocusEmulationEnabled", { enabled: true }, sessionId).catch(() => {});
    // Drags the browser would start from the page's elements are reported instead (headless Chrome lost its
    // connection in one); the driver's own file drops (hostdrop) are dispatched as drag events all the same.
    await send("Input.setInterceptDrags", { enabled: true }, sessionId).catch(() => {});

    const query = [];
    if (image) {
      query.push("image=" + image);
    }
    if (disk) {
      query.push("disk=" + disk);
    }
    if (pool) {
      query.push("pool=" + pool);
    }
    if (query_extra) {
      query.push(query_extra);
    }
    // --profile: the Workers, as they start (Workers that Workers start too), waiting for us until the profiler is ready.
    const workers = new Map();	// session -> { title, url, t0 }
    let profiling = false;
    const t_start = Date.now();
    if (profile_us) {
      const attach = { autoAttach: true, waitForDebuggerOnStart: true, flatten: true };
      event_hooks.push((msg) => {
        if (msg.method === "Target.attachedToTarget") {
          const w = msg.params.sessionId, info = msg.params.targetInfo;
          workers.set(w, { title: info.title, url: info.url, type: info.type, t0: Date.now() - t_start });
          (async () => {
            await ask("Profiler.enable", {}, w, 5000);
            await ask("Profiler.setSamplingInterval", { interval: profile_us }, w, 5000);
            await ask("Target.setAutoAttach", attach, w, 5000);
            if (profiling) {
              workers.get(w).started = !!await ask("Profiler.start", {}, w, 5000);
            }
            await ask("Runtime.runIfWaitingForDebugger", {}, w, 5000);
          })();
        } else if (msg.method === "Target.detachedFromTarget" && workers.has(msg.params.sessionId)) {
          workers.get(msg.params.sessionId).gone = Date.now() - t_start;
        }
      });
      await send("Target.setAutoAttach", attach, sessionId);
    }
    // A request to a Worker that may never answer (one that is gone, or never ran): null after `ms`.
    const ask = (method, params, w, ms) => Promise.race([send(method, params, w).catch(() => null),
                                                         sleep(ms).then(() => null)]);
    let worker_error_count = 0;
    const live_workers = new Set();	// (--worker-errors) Workers attached to and not yet gone; the `workers` action prints how many
    if (worker_errors && !profile_us) {
      const attach = { autoAttach: true, waitForDebuggerOnStart: true, flatten: true };
      event_hooks.push((msg) => {
        if (msg.method === "Target.detachedFromTarget") {
          live_workers.delete(msg.params.sessionId);
        }
        if (msg.method === "Target.attachedToTarget") {
          const w = msg.params.sessionId;
          live_workers.add(w);
          (async () => {
            await ask("Runtime.enable", {}, w, 5000);
            await ask("Target.setAutoAttach", attach, w, 5000);
            await ask("Runtime.runIfWaitingForDebugger", {}, w, 5000);
          })();
        } else if (msg.method === "Runtime.exceptionThrown" && msg.sessionId && msg.sessionId !== sessionId) {
          const d = msg.params.exceptionDetails;
          worker_error_count++;
          console.log("---- a Worker threw: " + ((d.exception && d.exception.description) || d.text).split("\n").slice(0, 4).join(" | "));
        }
      });
      await send("Target.setAutoAttach", attach, sessionId);
    }
    // A Worker's profile as counts by function: own samples, and samples with the function anywhere in the stack.
    const fold = (profile) => {
      const self = {}, incl = {};
      const nodes = new Map(profile.nodes.map((n) => [n.id, n]));
      const count = new Map();
      for (const id of profile.samples) {
        count.set(id, (count.get(id) || 0) + 1);
      }
      const keyOf = (n) => {
        const f = n.callFrame;
        let url = f.url || "";
        url = url.startsWith("wasm://") ? "wasm:" + url.replace(/^wasm:\/\/wasm\//, "").replace(/-[0-9a-f]{6,}$/, "").replace(/^[0-9a-f]{6,}$/, "")
          : url.replace(/^.*\//, "").replace(/\?.*$/, "");
        return (f.functionName || "(anonymous)") + " " + url;
      };
      // Inclusive counts: a walk down the tree with the names on the way there.
      const total = new Map();
      const sum = (n) => {
        let t = count.get(n.id) || 0;
        for (const c of n.children || []) {
          t += sum(nodes.get(c));
        }
        total.set(n.id, t);
        return t;
      };
      const root = profile.nodes[0];
      sum(root);
      const walk = (n, onPath) => {
        const k = keyOf(n), c = count.get(n.id) || 0, t = total.get(n.id);
        if (c) {
          self[k] = (self[k] || 0) + c;
        }
        let path = onPath;
        if (!onPath.has(k) && t) {
          incl[k] = (incl[k] || 0) + t;
          path = new Set(onPath);
          path.add(k);
        }
        for (const ch of n.children || []) {
          const m = nodes.get(ch);
          if (total.get(m.id)) {
            walk(m, path);
          }
        }
      };
      walk(root, new Set());
      return { samples: profile.samples.length, self, incl };
    };
    await send("Page.navigate", { url: base + "/dev.html" + (query.length ? "?" + query.join("&") : "") }, sessionId);
    // (The page's script may not have run yet, on a busy computer.)
    for (let i = 0; i < 100 && !await evaluate("typeof linuxReady != 'undefined'").catch(() => false); i++) {
      await sleep(200);
    }
    await evaluate("linuxReady");
    // Typed like a person would, and left to run.
    await evaluate(`linuxType(${JSON.stringify(args[0] + "\r")})`);

    // A point of the screen, in the page's coordinates (the screen's pixels are the window server's when it
    // runs, else the frame buffer's).
    let origin = [0, 0];	// (win:MATCH: coordinates from then on are a window's)
    const at = async (x, y) => {
      x += origin[0];
      y += origin[1];
      const box = await evaluate(`(() => { const b = document.getElementById("screen").getBoundingClientRect();
        const c = document.getElementById("screen");
        const size = window.rvDisplay && rvDisplay.active() && rvDisplay.screenSize ? rvDisplay.screenSize() : [c.width, c.height];
        return [b.left, b.top, b.width / size[0], b.height / size[1]]; })()`);
      return { x: box[0] + (x + 0.5) * box[2], y: box[1] + (y + 0.5) * box[3] };
    };
    const mouse = (type, p, more = {}) => send("Input.dispatchMouseEvent", { type, x: p.x, y: p.y, ...more }, sessionId);
    const press = async (x, y, button, count = 1) => {
      const p = await at(x, y);
      await mouse("mouseMoved", p);
      await sleep(80);
      for (let i = 1; i <= count; i++) {
        await mouse("mousePressed", p, { button, clickCount: i });
        await sleep(60);
        await mouse("mouseReleased", p, { button, clickCount: i });
        await sleep(90);
      }
    };
    const key = async (type, code) => {
      // (The keys go to the screen, unless something of the page's own has them, as a person's would: the
      // launcher's search field, a popup that took the keys.)
      await evaluate(`(() => { const a = document.activeElement;
        if (!a || a == document.body) { document.getElementById("screen").focus(); } })()`);
      await send("Input.dispatchKeyEvent", { type, code, key: code }, sessionId);
    };
    const stroke = async (code, modifiers = []) => {
      for (const m of modifiers) {
        await key("rawKeyDown", m);
        await sleep(30);
      }
      await key("rawKeyDown", code);
      await sleep(40);
      await key("keyUp", code);
      for (const m of modifiers.slice().reverse()) {
        await sleep(30);
        await key("keyUp", m);
      }
      await sleep(40);
    };

    // What the kernel's own variables say (their addresses are in kernel-symbols.txt, from the kernel's build),
    // read twice a second apart: is the clock ticking, does any CPU have something to run, is a disk request
    // under way and for how long. For a machine that does not answer any more.
    const kernel_state = async () => {
      const read = `(async () => {
        const text = await (await fetch("kernel-symbols.txt", { cache: "no-store" })).text();
        const at = {};
        for (const line of text.split("\\n")) {
          const p = line.split(" ");
          if (p.length == 3) { at[p[2]] = parseInt(p[0], 16); }
        }
        const os = window.linuxOs;
        if (!os || !os.memory) { return "the page does not show its machine's memory"; }
        const look = () => {
          const m = new DataView(os.memory.buffer);
          const out = { jiffies: Number(m.getBigUint64(at.jiffies_64, true)), tick_cpu: m.getInt32(at.tick_do_timer_cpu, true), cpus: [] };
          const online = m.getBigUint64(at.__cpu_online_mask, true);
          for (let cpu = 0; cpu < 64; cpu++) {
            if (!((online >> BigInt(cpu)) & 1n)) { continue; }
            const base = Number(m.getBigUint64(at.__per_cpu_offset + 8 * cpu, true));
            const c = { cpu: cpu, running: m.getUint32(base + at.runqueues + 4, true) };
            if (at.raised_irqs !== undefined) { c.irqs = Number(m.getBigUint64(base + at.raised_irqs, true)); }
            if (at.local_timer_expiries !== undefined) { c.timer = Number(m.getBigInt64(base + at.local_timer_expiries, true)); }
            out.cpus.push(c);
          }
          out.disks = (os.disks || []).map((d) => d ? Array.from(d.slice(0, 9)) : null);
          return out;
        };
        const a = look();
        await new Promise((r) => setTimeout(r, 1000));
        const b = look();
        return JSON.stringify({ a: a, b: b });
      })()`;
      try {
        const { a, b } = JSON.parse(await evaluate(read));
        const lines = [];
        lines.push(`jiffies ${a.jiffies} -> ${b.jiffies} in a second (${b.jiffies == a.jiffies ? "THE CLOCK STANDS STILL" : "ticking"}); the CPU that keeps time: ${b.tick_cpu}`);
        lines.push("runnable tasks per CPU (cpu:tasks" + (b.cpus[0] && b.cpus[0].timer !== undefined ? ", t=next timer, i=pending interrupts" : "") + "): "
                   + b.cpus.map((c) => c.cpu + ":" + c.running + (c.timer !== undefined ? "(t=" + c.timer + ",i=" + c.irqs + ")" : "")).join(" "));
        b.disks.forEach((d, i) => {
          if (d) {
            lines.push(`disk ${i}: lock ${d[0]}, request ${d[1]}, done ${d[2]}, op ${d[3]}, sector ${(d[4] >>> 0) + d[5] * 4294967296}, count ${d[7]}, result ${d[8]}`
                       + (d[1] == 1 ? "  <- A REQUEST IS WAITING FOR THE DISK'S WORKER" : "")
                       + (JSON.stringify(a.disks[i]) == JSON.stringify(d) && d[0] ? "  (the same as a second ago)" : ""));
          }
        });
        return lines.join("\n");
      } catch (error) {
        return "could not be read: " + error.message;
      }
    };

    let dragFrom = null;
    let marked = Date.now();
    const act = async (action) => {
      const colon = action.indexOf(":");
      const what = colon < 0 ? action : action.slice(0, colon), value = colon < 0 ? "" : action.slice(colon + 1);
      const numbers = value.split(",").map(Number);
      if (what == "wait") {
        await sleep(Number(value) * 1000);
      } else if (what == "shot") {
        // (With the window server's windows over the screen, display.js composes the picture.)
        const data = await evaluate(`window.rvDisplay && rvDisplay.active() ? rvDisplay.shot() : document.getElementById("screen").toDataURL("image/png")`);
        fs.writeFileSync(value, Buffer.from(data.slice(data.indexOf(",") + 1), "base64"));
        console.log("shot " + value);
      } else if (what == "move") {
        await mouse("mouseMoved", await at(numbers[0], numbers[1]));
      } else if (what == "click" || what == "rclick" || what == "mclick" || what == "dclick" || what == "tclick") {
        await press(numbers[0], numbers[1], what == "rclick" ? "right" : what == "mclick" ? "middle" : "left",
                    what == "dclick" ? 2 : what == "tclick" ? 3 : 1);
      } else if (what == "drag") {
        const from = await at(numbers[0], numbers[1]), to = await at(numbers[2], numbers[3]);
        await mouse("mouseMoved", from);
        await mouse("mousePressed", from, { button: "left", clickCount: 1 });
        for (let i = 1; i <= 10; i++) {
          await sleep(40);
          await mouse("mouseMoved", { x: from.x + (to.x - from.x) * i / 10, y: from.y + (to.y - from.y) * i / 10 },
                      { button: "left", buttons: 1 });
        }
        await mouse("mouseReleased", to, { button: "left", clickCount: 1 });
      } else if (what == "press") {
        const p = await at(numbers[0], numbers[1]);
        await mouse("mouseMoved", p);
        await sleep(100);
        await mouse("mousePressed", p, { button: "left", clickCount: 1 });
        await sleep(400);
        dragFrom = p;
      } else if (what == "pressel") {
        // press:, at the middle of the page's element that matches a selector (a window's title bar, say).
        const box = await evaluate(`(() => { const e = document.querySelector(${JSON.stringify(value)});
          if (!e) return null; const b = e.getBoundingClientRect(); return [b.left + b.width / 2, b.top + b.height / 2]; })()`);
        if (!box) {
          console.log("pressel: nothing matches " + value);
        } else {
          const p = { x: box[0], y: box[1] };
          await mouse("mouseMoved", p);
          await sleep(100);
          await mouse("mousePressed", p, { button: "left", clickCount: 1 });
          await sleep(400);
          dragFrom = p;
          console.log("pressed " + value + " at " + Math.round(p.x) + "," + Math.round(p.y));
        }
      } else if (what == "moveto") {
        const to = await at(numbers[0], numbers[1]), steps = numbers[2] || 30, from = dragFrom || to;
        for (let i = 1; i <= steps; i++) {
          // (Small steps first: a program starts a drag once the pointer has gone a few pixels.)
          const f = i <= 5 ? i * 0.005 : 0.025 + (i - 5) / (steps - 5) * 0.975;
          await mouse("mouseMoved", { x: from.x + (to.x - from.x) * f, y: from.y + (to.y - from.y) * f }, { button: "left", buttons: 1 });
          await sleep(50);
        }
        dragFrom = to;
      } else if (what == "release") {
        const p = await at(numbers[0], numbers[1]);
        await mouse("mouseReleased", p, { button: "left", clickCount: 1 });
        dragFrom = null;
      } else if (what == "type") {
        // (Into one of the page's own fields, the launcher's search say, keys make text only with their text.)
        const field = await evaluate(`(() => { const a = document.activeElement;
          return !!a && a.matches("input:not([type=checkbox]), textarea:not(.rv-ime)"); })()`);
        for (const c of value) {
          if (!(c in KEYS)) {
            throw new Error("no key for " + JSON.stringify(c));
          }
          if (field && c != "\n" && c != "\t") {
            await send("Input.dispatchKeyEvent", { type: "keyDown", code: KEYS[c][0], key: c, text: c }, sessionId);
            await send("Input.dispatchKeyEvent", { type: "keyUp", code: KEYS[c][0], key: c }, sessionId);
            await sleep(40);
            continue;
          }
          await stroke(KEYS[c][0], KEYS[c][1] ? ["ShiftLeft"] : []);
        }
      } else if (what == "key") {
        const parts = value.split("+");
        const name = parts.pop();
        await stroke(MODIFIERS[name] || name, parts.map((m) => {
          if (!(m in MODIFIERS)) {
            throw new Error("no modifier " + m);
          }
          return MODIFIERS[m];
        }));
      } else if (what == "fkey") {
        // As a browser sends it to a page that reads the key's name, its code and the modifiers' flags (the
        // screen's handler does not need them: it keeps the keys' state itself).
        const parts = value.split("+");
        const code = parts.pop();
        const flags = parts.reduce((f, m) => f | ({ alt: 1, ctrl: 2, meta: 4, shift: 8 }[m] || 0), 0);
        const ev = (type, c, k, vk, mods, text) => send("Input.dispatchKeyEvent", { type, code: c, key: k, windowsVirtualKeyCode: vk,
          nativeVirtualKeyCode: vk, modifiers: mods, ...(text ? { text, unmodifiedText: text } : {}) }, sessionId);
        const named = { Enter: [13, "\r"], Escape: [27], Tab: [9], Space: [32, " "], Backspace: [8], Delete: [46],
                        ArrowLeft: [37], ArrowUp: [38], ArrowRight: [39], ArrowDown: [40], Home: [36], End: [35] };
        let k = code, vk = 0, text = "";
        if (/^Key[A-Z]$/.test(code)) {
          k = (flags & 8) ? code[3] : code[3].toLowerCase();
          vk = code.charCodeAt(3);
          text = flags & 7 ? "" : k;
        } else if (/^Digit[0-9]$/.test(code)) {
          k = code[5];
          vk = code.charCodeAt(5);
          text = flags & 7 ? "" : k;
        } else if (/^Numpad[0-9]$/.test(code)) {
          k = code[6];
          vk = 96 + Number(code[6]);
          text = flags & 7 ? "" : k;
        } else if (/^F[0-9]+$/.test(code)) {
          vk = 111 + Number(code.slice(1));
        } else if (named[code]) {
          k = code === "Space" ? " " : code;
          vk = named[code][0];
          text = flags & 7 ? "" : named[code][1] || "";
        }
        const mods = [["alt", "AltLeft", "Alt", 18, 1], ["ctrl", "ControlLeft", "Control", 17, 2], ["meta", "MetaLeft", "Meta", 91, 4],
                      ["shift", "ShiftLeft", "Shift", 16, 8]].filter((m) => flags & m[4]);
        let held = 0;
        for (const m of mods) {
          held |= m[4];
          await ev("rawKeyDown", m[1], m[2], m[3], held);
          await sleep(30);
        }
        await ev(text ? "keyDown" : "rawKeyDown", code, k, vk, flags, text);
        await sleep(60);
        await ev("keyUp", code, k, vk, flags);
        for (const m of mods.slice().reverse()) {
          await sleep(30);
          held &= ~m[4];
          await ev("keyUp", m[1], m[2], m[3], held);
        }
        await sleep(40);
      } else if (what == "mdrag") {
        // The middle button held from one point to the other, in steps (Blender orbits its view so).
        const from = await at(numbers[0], numbers[1]), to = await at(numbers[2], numbers[3]);
        await mouse("mouseMoved", from);
        await sleep(100);
        await mouse("mousePressed", from, { button: "middle", buttons: 4, clickCount: 1 });
        for (let i = 1; i <= 20; i++) {
          await sleep(50);
          await mouse("mouseMoved", { x: from.x + (to.x - from.x) * i / 20, y: from.y + (to.y - from.y) * i / 20 },
                      { button: "middle", buttons: 4 });
        }
        await mouse("mouseReleased", to, { button: "middle", clickCount: 1 });
      } else if (what == "sh") {
        const r = await evaluate(`linuxRun(${JSON.stringify(value)}, ${sh_timeout})`);
        console.log(`[${r.rc === null ? "NOT DONE" : "rc " + r.rc}, ${r.ms} ms] ${value}\n${r.out}`);
        if (r.rc === null) {
          // It hangs: interrupt it, and look at the machine with programs that are not the emulator's.
          await evaluate(`linuxType("\\x03")`);
          await sleep(1500);
          const look = await evaluate(`linuxRun(${JSON.stringify(DIAGNOSE)}, 60000)`);
          console.log(`---- the machine, after that (${look.rc === null ? "this hangs too" : look.ms + " ms"}) ----\n${look.out}`);
          console.log("---- the page's log ----\n" + await evaluate(`linuxLog.slice(-12).join("\\n")`));
          console.log("---- the kernel, read out of the machine's memory ----\n" + await kernel_state());
          if (on_hang) {
            console.log("---- " + on_hang + " ----\n" + await evaluate(fs.readFileSync(on_hang, "utf8")).catch((error) => "failed: " + error.message));
          }
          hung = true;
        }
      } else if (what == "win") {
        // The window server's window whose title or app id has MATCH in it (the topmost such): the
        // coordinates of the actions after this one are its own, from the top left of its contents.
        const found = value ? await evaluate(`(() => { const m = ${JSON.stringify(value)};
          const all = window.rvDisplay ? rvDisplay.windows().filter((w) => (w.title || "").includes(m) || (w.appId || "").includes(m)) : [];
          all.sort((a, b) => b.z - a.z);
          return all[0] || null; })()`) : null;
        origin = found ? [found.x, found.y] : [0, 0];
        console.log("win " + value + ": " + (found ? `"${found.title}" (${found.appId}) at ${found.x},${found.y}, ${found.w}x${found.h}` : "none (screen coordinates)"));
      } else if (what == "pageshot") {
        const box = await evaluate(`(() => { const b = document.getElementById("screen").getBoundingClientRect();
          return [b.left + window.scrollX, b.top + window.scrollY, b.width, b.height]; })()`);
        const r = await send("Page.captureScreenshot", { format: "png", captureBeyondViewport: true,
          clip: { x: box[0], y: box[1], width: box[2], height: box[3], scale: 1 } }, sessionId);
        fs.writeFileSync(value, Buffer.from(r.data, "base64"));
        console.log("pageshot " + value);
      } else if (what == "clickel") {
        // A person's click (the browser's own events) in the middle of the page's element that matches a selector.
        const box = await evaluate(`(() => { const e = document.querySelector(${JSON.stringify(value)});
          if (!e) return null; const b = e.getBoundingClientRect(); return [b.left + b.width / 2, b.top + b.height / 2]; })()`);
        if (!box) {
          console.log("clickel: nothing matches " + value);
        } else {
          const p = { x: box[0], y: box[1] };
          await mouse("mouseMoved", p);
          await sleep(80);
          await mouse("mousePressed", p, { button: "left", clickCount: 1 });
          await sleep(60);
          await mouse("mouseReleased", p, { button: "left", clickCount: 1 });
          console.log("clicked " + value + " at " + Math.round(p.x) + "," + Math.round(p.y));
        }
      } else if (what == "tabs") {
        // The browser's pages (tabs) now: what the page has opened (window.open, a link with target=_blank).
        const { targetInfos } = await send("Target.getTargets");
        console.log("tabs: " + JSON.stringify(targetInfos.filter((t) => t.type == "page").map((t) => t.url)));
      } else if (what == "hostclip") {
        await evaluate(`navigator.clipboard.writeText(${JSON.stringify(value)}).then(() => "ok")`);
        console.log("host clipboard set: " + JSON.stringify(value));
      } else if (what == "readclip") {
        const text = await evaluate(`navigator.clipboard.readText()`).catch((error) => "(failed: " + error.message + ")");
        console.log("host clipboard: " + JSON.stringify(text));
      } else if (what == "viewport") {
        await send("Emulation.setDeviceMetricsOverride", { width: numbers[0], height: numbers[1],
          deviceScaleFactor: numbers[2] || 1, mobile: false }, sessionId);
        console.log("viewport " + numbers[0] + "x" + numbers[1] + " at " + (numbers[2] || 1));
      } else if (what == "keydown" || what == "keyup") {
        await key(what == "keydown" ? "rawKeyDown" : "keyUp", MODIFIERS[value] || value);
        await sleep(40);
      } else if (what == "hostdrop") {
        const parts = value.split(",");
        const file = path.resolve(parts[0]);
        const p = parts.length > 2 ? await at(Number(parts[1]), Number(parts[2])) : await at(10, 10);
        const data = { items: [], files: [file], dragOperationsMask: 1 };
        const before = await evaluate(`(rvDisplay.stats.drops || []).length`);
        for (const type of ["dragEnter", "dragOver", "drop"]) {
          await send("Input.dispatchDragEvent", { type, x: p.x, y: p.y, data }, sessionId);
          await sleep(100);
        }
        let drops = [];
        for (let i = 0; i < 300; i++) {
          drops = await evaluate(`rvDisplay.stats.drops || []`);
          if (drops.length > before) {
            break;
          }
          await sleep(200);
        }
        console.log("hostdrop " + file + ": " + JSON.stringify(drops.slice(before)));
      } else if (what == "until") {
        // until:SECONDS:EXPRESSION  wait until the page's EXPRESSION is true (at most SECONDS); says how long it took
        const c = value.indexOf(":"), limit = Number(value.slice(0, c)) * 1000, expr = value.slice(c + 1);
        const t0 = Date.now();
        let ok = false;
        while (Date.now() - t0 < limit) {
          if (await evaluate(`!!(${expr})`).catch(() => false)) {
            ok = true;
            break;
          }
          await sleep(200);
        }
        console.log("until " + (ok ? "true" : "STILL FALSE") + " after " + (Date.now() - t0) + " ms: " + expr.slice(0, 80));
      } else if (what == "wheel") {
        // wheel:X,Y,DY: the wheel turned over X,Y (DY > 0 scrolls down; 100 is about a notch)
        const p = await at(numbers[0], numbers[1]);
        await mouse("mouseMoved", p);
        await mouse("mouseWheel", p, { deltaX: 0, deltaY: numbers[2] || 100 });
      } else if (what == "insert") {
        // insert:TEXT: text as an input method would commit it (Input.insertText), for text no key makes (CJK)
        await evaluate(`(() => { const a = document.activeElement;
          if (!a || a == document.body) { document.getElementById("screen").focus(); } })()`);
        await send("Input.insertText", { text: value }, sessionId);
      } else if (what == "compose") {
        // compose:TEXT  an input method composing TEXT (composition events, as the Mac's Japanese input makes
        // them while one types), then committing it
        for (let i = 1; i <= [...value].length; i++) {
          const part = [...value].slice(0, i).join("");
          await send("Input.imeSetComposition", { text: part, selectionStart: part.length, selectionEnd: part.length }, sessionId);
          await sleep(60);
        }
        await send("Input.insertText", { text: value }, sessionId);
      } else if (what == "kernel") {
        console.log("---- the kernel ----\n" + await kernel_state());
      } else if (what == "workers") {
        console.log("workers: " + live_workers.size + " (needs --worker-errors)");
      } else if (what == "mark") {
        marked = Date.now();
      } else if (what == "waitwin" || what == "waittitle") {
        const comma = value.lastIndexOf(",");
        const has_s = comma > 0 && /^\d+$/.test(value.slice(comma + 1));
        const re = has_s ? value.slice(0, comma) : value, limit = has_s ? Number(value.slice(comma + 1)) : 600;
        const field = what == "waitwin" ? "w.title + ' ' + w.appId" : "w.title";
        const found = await evaluate(`(async () => { const re = new RegExp(${JSON.stringify(re)}, "i");
          const t0 = performance.now();
          while (performance.now() - t0 < ${limit * 1000}) {
            const w = window.rvDisplay && rvDisplay.windows().find((w) => w.kind === 1 && re.test(${field}));
            if (w) { return JSON.stringify({ title: w.title, app: w.appId, w: w.w, h: w.h, present: w.present }); }
            await new Promise((r) => setTimeout(r, 50));
          }
          return null; })()`);
        console.log(`${what} ${re}: ${found ? ((Date.now() - marked) / 1000).toFixed(1) + " s after the mark: " + found : "NOT THERE after " + limit + " s"}`);
      } else if (what == "profstart") {
        profiling = true;
        const n = [...workers.keys()].filter((w) => !workers.get(w).gone);
        const t0 = Date.now();
        const ok = await Promise.all(n.map(async (w) => (workers.get(w).started = !!await ask("Profiler.start", {}, w, 10000))));
        console.log(`profile: started in ${ok.filter(Boolean).length} of ${n.length} Workers (${Date.now() - t0} ms)`);
      } else if (what == "profstop") {
        profiling = false;
        const out = { interval_us: profile_us, workers: [] };
        const t0 = Date.now();
        await Promise.all([...workers.keys()].map(async (w) => {
          const info = workers.get(w);
          if (info.gone || !info.started) {
            return;
          }
          const r = await ask("Profiler.stop", {}, w, 30000);
          if (r && r.profile) {
            out.workers.push({ ...info, ...fold(r.profile) });
          }
        }));
        fs.writeFileSync(value, JSON.stringify(out));
        console.log(`profile: ${out.workers.length} Workers, ${out.workers.reduce((a, w) => a + w.samples, 0)} samples, `
                    + `in ${value} (${Date.now() - t0} ms to collect)`);
      } else if (what == "js") {
        // js:EXPRESSION, evaluated in the page (awaited if it is a promise), its value printed
        console.log("js " + value.slice(0, 60) + ": " + await evaluate(value).catch((error) => "failed: " + error.message));
      } else if (what == "eval") {
        console.log("---- " + value + " ----\n" + await evaluate(fs.readFileSync(value, "utf8")).catch((error) => "failed: " + error.message));
      } else if (what == "log") {
        console.log(await evaluate(`linuxLog.slice(-${Number(value) || 20}).join("\\n")`));
      } else {
        throw new Error("no such action: " + action);
      }
    };
    for (const action of args.slice(1)) {
      if (action.startsWith("repl:")) {
        await repl(action.slice(5), act);
      } else {
        await act(action);
      }
    }
    done = true;
    ws.close();
    if (worker_errors && !profile_us) {
      console.log("worker errors: " + worker_error_count);
      if (worker_error_count) {
        status = 6;
      }
    }
    if (hung) {
      status = 4;
    }
  } catch (error) {
    console.error(error.message);
    status = 1;
  }
  chrome.kill();
  await sleep(300);
  if (process.env.KEEP_PROFILE) {
    console.error("the browser's profile is left in " + profile);
  } else {
    fs.rmSync(profile, { recursive: true, force: true });
  }
  process.exit(status);
}

main();
