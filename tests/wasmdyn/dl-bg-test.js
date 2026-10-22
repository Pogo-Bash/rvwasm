#!/usr/bin/env node
// The page's background compile (tools/www/dev.html, between BEGIN dl-background and END dl-background) against a mock machine: the
// order, one at a time, waiting for the desktop and for quiet, pausing while a program loads libraries, skipping what a program took
// meanwhile, keeping Modules below keepBelow and warming the rest. Needs only node.
const fs = require("fs");
const html = fs.readFileSync(__dirname + "/../../tools/www/dev.html", "utf8");
const text = html.slice(html.indexOf("// BEGIN dl-background"), html.indexOf("// END dl-background"));
const dlBackground = new Function(text + "\nreturn { dlBackground, DL_BACKGROUND_START_QUIET_MS, DL_BACKGROUND_QUIET_MS };")();
let failed = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) failed++; };

(async () => {
  const { dlBackground: run, DL_BACKGROUND_START_QUIET_MS: START, DL_BACKGROUND_QUIET_MS: QUIET } = dlBackground;
  const libs = [
    { key: "a", name: "a", url: "a.wasm", size: 10, preload: true, order: 0 },
    { key: "b", name: "b", url: "b.wasm", size: 5, preload: false, order: 2 },
    { key: "c", name: "c", url: "c.wasm", size: 5, preload: false, order: 1 },
    { key: "d", name: "d", url: "d.wasm", size: 9, preload: false },
    { key: "e", name: "e", url: "e.wasm", size: 3, preload: false },
    { key: "f", name: "f", url: "f.wasm", size: 5, preload: false, order: 3 },
  ];
  let clock = 0, windows = 0, bytes = 0;
  const tick = (seconds) => { for (let i = 0; i < seconds; i++) { clock += 1000; scenario(clock); } };
  const events = [];
  const modules = new Set(["a"]);
  const dllog = [];
  const os = { dl: {
    log: dllog, keys: () => [...modules],
    // (a compile takes 2 s of the machine's time)
    preload: async (es) => { for (const e of es) { events.push("keep:" + e.key + "@" + clock); modules.add(e.key); tick(2); } },
    warm: async (e) => { events.push("warm:" + e.key + "@" + clock); tick(2); },
  } };
  // The machine: the desktop appears at 5 s, then drawing for a while; a program starts at 19 s, loads libraries (takes e itself) and
  // draws its window until 22 s; the tab is hidden 26-30 s.
  function scenario(t) {
    windows = t >= 5000 ? 1 : 0;
    if (t < 9000) bytes += 2000000;            // desktop drawing
    if (t === 19000) { dllog.push({ key: "x", how: "compile", runner: "r1" }); modules.add("e"); }
    if (t >= 19000 && t < 22000) bytes += 1000000;   // its window drawing
  }
  const out = await run(os, "dl/libs.json", {
    base: "https://x.example/", keepBelow: 3,
    fetchJson: async () => ({ libs }),
    sleep: async (ms) => tick(ms / 1000),
    now: () => clock,
    hidden: () => clock >= 26000 && clock < 30000,
    display: () => ({ bytes, windows }),
    log: (m) => events.push(m),
  });
  console.log(events.join("\n"));
  const evs = events.filter((e) => /^(keep|warm):/.test(e));
  const names = evs.map((e) => e.split(":")[1].split("@")[0]);
  const times = evs.map((e) => Number(e.split("@")[1]));
  check(names.join("") === "cbfd", "order by the manifest's order (c b f), the unordered last (d), e (taken by a program) skipped: " + names.join(""));
  check(!names.includes("a"), "a preloaded one is not touched");
  check(times[0] >= 9000 + START - 2000, "nothing before the desktop is up and quiet (first at " + times[0] + " ms)");
  check(times.every((t) => !(t >= 19000 && t < 21000 + QUIET)), "nothing while a program is loading and drawing, and QUIET after (19000-" + (21000 + QUIET) + " ms)");
  check(times.every((t) => !(t >= 26000 && t < 30000)), "nothing while the tab is hidden (26000-30000 ms)");
  check(evs.some((e) => e.startsWith("keep:c")) && evs.some((e) => e.startsWith("keep:b")) && evs.some((e) => e.startsWith("warm:f")),
        "order below keepBelow (3) kept, the rest warmed");
  check(out.done === 4 && out.kept === 2, "counts " + JSON.stringify(out));
  process.exit(failed ? 1 : 0);
})();
