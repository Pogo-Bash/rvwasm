#!/usr/bin/env node
"use strict";
// tools/gpu-nm.sh's last step: what a run did, in a few lines, and a verdict (exit 0 / 1).
//   node gpu/tools/gpu-nm-report.js PROGRAM NODE_MACHINE_RC OUT_FILE ERR_FILE SHOTS_DIR
//   node gpu/tools/gpu-nm-report.js --probe FILE.png [es2gears]     the pixel probe alone
// OUT_FILE: what the program printed in the machine; ERR_FILE: what node-machine and the hooks said (stderr).
//
// Pixel probe (es2gears): the window's PNG (tools/wl-dump.js: the client's buffer) is classified pixel by pixel:
// red (the red gear, 0.8 0.1 0.0 lit), green (0.0 0.8 0.2), blue (0.2 0.2 1.0) and background (the black clear colour). It
// passes when each gear colour covers at least 0.4% of the window, the background at least 20%, and nothing else
// (the "wrong" class: pixels that are none of these) more than 25%. A window the GPU never drew (a flat colour, the
// stub's changing colour, grey) has no red, green and blue at once.
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

function readPng(file) {
  const b = fs.readFileSync(file);
  let at = 8, w = 0, h = 0, depth = 0, type = 0;
  const idat = [];
  while (at < b.length) {
    const len = b.readUInt32BE(at), kind = b.toString("ascii", at + 4, at + 8), data = b.subarray(at + 8, at + 8 + len);
    if (kind === "IHDR") {
      w = data.readUInt32BE(0);
      h = data.readUInt32BE(4);
      depth = data[8];
      type = data[9];
    } else if (kind === "IDAT") {
      idat.push(data);
    }
    at += 12 + len;
  }
  if (depth !== 8 || (type !== 6 && type !== 2)) throw new Error("png: only 8-bit RGB/RGBA (" + file + ")");
  const bpp = type === 6 ? 4 : 3;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const out = Buffer.alloc(w * h * 4);
  const prev = Buffer.alloc(w * bpp), line = Buffer.alloc(w * bpp);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (w * bpp + 1)];
    for (let x = 0; x < w * bpp; x++) {
      const v = raw[y * (w * bpp + 1) + 1 + x];
      const a = x >= bpp ? line[x - bpp] : 0, up = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
      let r;
      switch (f) {
        case 0: r = v; break;
        case 1: r = v + a; break;
        case 2: r = v + up; break;
        case 3: r = v + ((a + up) >> 1); break;
        default: {
          const p = a + up - c, pa = Math.abs(p - a), pb = Math.abs(p - up), pc = Math.abs(p - c);
          r = v + (pa <= pb && pa <= pc ? a : pb <= pc ? up : c);
        }
      }
      line[x] = r & 255;
    }
    line.copy(prev);
    for (let x = 0; x < w; x++) {
      out[(y * w + x) * 4] = line[x * bpp];
      out[(y * w + x) * 4 + 1] = line[x * bpp + 1];
      out[(y * w + x) * 4 + 2] = line[x * bpp + 2];
      out[(y * w + x) * 4 + 3] = bpp === 4 ? line[x * bpp + 3] : 255;
    }
  }
  return { w, h, rgba: out };
}

function gearsProbe(file) {
  const { w, h, rgba } = readPng(file);
  const c = { red: 0, green: 0, blue: 0, background: 0, other: 0 };
  for (let i = 0; i < w * h; i++) {
    const r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
    if (r < 24 && g < 24 && b < 24) c.background++;
    else if (r > 60 && r > g * 1.8 && r > b * 1.8) c.red++;
    else if (g > 60 && g > r * 1.5 && g > b * 1.2) c.green++;
    else if (b > 60 && b > r * 1.5 && b > g * 1.5) c.blue++;
    else c.other++;
  }
  const n = w * h, pct = (v) => (100 * v / n).toFixed(1) + "%";
  const ok = c.red >= 0.004 * n && c.green >= 0.004 * n && c.blue >= 0.004 * n && c.background >= 0.2 * n && c.other <= 0.25 * n;
  return { ok, text: path.basename(file) + " " + w + "x" + h + ": red " + pct(c.red) + ", green " + pct(c.green) + ", blue " + pct(c.blue)
                     + ", background " + pct(c.background) + ", other " + pct(c.other), counts: c };
}

// vkcube: the LunarG cube has teal (r < 40, g > 35, b > 40, b >= g: the logo's dark cyan) and light grey faces (r > 85) on a dark grey clear colour; a
// frame that is only the clear colour (nothing drawn) or one flat colour has none of the teal.
function cubeProbe(file) {
  const { w, h, rgba } = readPng(file);
  let teal = 0, light = 0;
  for (let i = 0; i < w * h; i++) {
    const r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
    if (r < 40 && g > 35 && b > 40 && b >= g) teal++;
    else if (r > 85 && Math.abs(r - g) < 12 && Math.abs(r - b) < 12) light++;
  }
  const n = w * h;
  return { ok: teal >= 0.01 * n && light >= 0.02 * n, text: path.basename(file) + " " + w + "x" + h + ": teal " + (100 * teal / n).toFixed(1) + "%, light " + (100 * light / n).toFixed(1) + "%" };
}

// mpv with the page's video path (VK-14): gpu/tools/vdec-stub.js's test card as mpv shows it. Eight colour bars in the top 45% (white,
// yellow, cyan, green, magenta, red, blue, black: each of R, G, B above or below 128), a grey ramp from 45% to 70% (dark at the left, light at
// the right) and black below it (but for a white marker). mpv's own colour handling may shift the values, so the test is on which side of
// 128 each channel falls, and on the ramp rising to the right, which also tells the picture is the right way up.
function cardProbe(file) {
  const { w, h, rgba } = readPng(file);
  const at = (fx, fy) => {
    const i = (Math.floor(fy * h) * w + Math.floor(fx * w)) * 4;
    return [rgba[i], rgba[i + 1], rgba[i + 2]];
  };
  const want = [[1, 1, 1], [1, 1, 0], [0, 1, 1], [0, 1, 0], [1, 0, 1], [1, 0, 0], [0, 0, 1], [0, 0, 0]];
  let bars = 0;
  for (let i = 0; i < 8; i++) {
    const c = at((i + 0.5) / 8, 0.22);
    if (c.every((v, k) => (v > 128 ? 1 : 0) === want[i][k])) bars++;
  }
  const lum = (c) => (c[0] + c[1] + c[2]) / 3;
  const left = lum(at(0.12, 0.58)), right = lum(at(0.88, 0.58)), below = lum(at(0.5, 0.95));
  const ramp = right - left > 100 && Math.abs(lum(at(0.5, 0.58)) - 128) < 50;
  return { ok: bars === 8 && ramp && below < 40, text: path.basename(file) + " " + w + "x" + h + ": " + bars + " of 8 bars, ramp " + Math.round(left)
                     + " -> " + Math.round(right) + ", bottom " + Math.round(below) };
}

// A window of an app whose picture is not known in advance (Qt Quick, GTK 4, Chromium: VK-17): drawn, i.e. not one flat colour and not
// a near-flat one. The share of the most common colour (quantised to 4 bits a channel) is at most 92%, and there are at least 6 colours.
function drawnProbe(file) {
  const { w, h, rgba } = readPng(file);
  const count = new Map();
  for (let i = 0; i < w * h; i++) {
    const k = ((rgba[i * 4] >> 4) << 8) | ((rgba[i * 4 + 1] >> 4) << 4) | (rgba[i * 4 + 2] >> 4);
    count.set(k, (count.get(k) || 0) + 1);
  }
  let top = 0;
  for (const v of count.values()) if (v > top) top = v;
  const n = w * h;
  return { ok: count.size >= 6 && top <= 0.92 * n, text: path.basename(file) + " " + w + "x" + h + ": " + count.size + " colours, the commonest " + (100 * top / n).toFixed(1) + "%",
           hash: require("crypto").createHash("sha1").update(rgba).digest("hex").slice(0, 10) };
}

function main() {
  if (process.argv[2] === "--probe" || process.argv[2] === "--probe-cube") {
    const r = (process.argv[2] === "--probe" ? gearsProbe : cubeProbe)(process.argv[3]);
    console.log((r.ok ? "PASS " : "FAIL ") + r.text);
    process.exit(r.ok ? 0 : 1);
  }
  const [prog, rcText, outFile, errFile, shots] = process.argv.slice(2);
  const rc = Number(rcText);
  const out = fs.existsSync(outFile) ? fs.readFileSync(outFile, "utf8") : "";
  const err = fs.existsSync(errFile) ? fs.readFileSync(errFile, "utf8") : "";
  const lines = [];
  const checks = [];
  const warns = [];
  const say = (k, v) => lines.push((k + ":").padEnd(10) + v);
  const check = (name, ok, detail) => checks.push({ name, ok, detail });
  // (what tools/gpu-matrix.sh puts in its table: the last line of the report, MATRIX_RESULT {json})
  const R = { app: prog };

  const sm = err.match(/\[rvgpu-nm\] summary (\{.*\})/);
  let s = null;
  try { s = sm ? JSON.parse(sm[1]) : null; } catch (e) { /* below */ }
  const stub = /gpu-stub: ring at/.test(err);
  say("program", prog + (stub ? "  (STUB executor: nothing is drawn)" : ""));
  if (s) {
    say("stamp", s.stamp || "(none)");
    R.stamp = s.stamp || null;
    R.submits = s.ring ? s.ring.submitted : 0;
    R.presents = s.presents || 0;
    say("executor", (s.renderer || "(did not start)") + "; caps " + (s.capsLen || 0) + " bytes; ready after " + (s.readyMs || "?") + " ms");
    if (s.ring) say("ring", "submitted " + s.ring.submitted + ", done " + s.ring.done + ", heartbeat " + s.ring.heartbeat);
    if (s.waits !== undefined) say("jspi waits", s.waits + " wgvk_wait calls (each a JSPI suspension of rvgpu_ring_drain; counted from the wasm debug words)");
    if (s.lazy) say("lazy", s.lazy + (s.lazyStartMs ? "; the executor took " + s.lazyStartMs + " ms from gpuserver's `want` to ready" : ""));
    if (s.deviceLost) say("lost", s.deviceLost);
    if (s.presents) say("presents", s.presents + " (stage B frames from the executor), last " + s.lastPresent + ", " + s.shots + " PNGs");
    if (s.errors && s.errors.length) say("errors", s.errors.slice(0, 5).join(" | ").slice(0, 600));
    check("executor ready", !!s.renderer && !s.fallback, s.renderer || "no");
    check("ring moved", !!(s.ring && s.ring.submitted > 0), s.ring ? s.ring.submitted + " submits" : "no ring");
  } else if (!stub) {
    say("executor", "no summary line (the hook did not finish: see " + errFile + ")");
    check("executor ready", false, "no summary");
  }
  const booted = err.match(/booted in (\d+) ms/);
  if (booted) say("boot", booted[1] + " ms");
  const gpuServer = out.match(/^.*(?:executor is here|rvgpu: [^\n]*executor)[^\n]*$/m) || err.match(/^.*gpuserver[^\n]*$/m);
  if (gpuServer) say("gpuserver", gpuServer[0].trim().slice(0, 200));

  // the program's own numbers
  const fps = [...out.matchAll(/(\d+) frames in ([\d.]+) seconds = ([\d.]+) FPS/g)];
  if (fps.length) {
    const frames = fps.reduce((n, m) => n + Number(m[1]), 0);
    say("frames", frames + " in " + fps.length + " reports of es2gears; last " + fps[fps.length - 1][0]);
    R.frames = frames;
    R.fps = Number(fps[fps.length - 1][3]);
    check("frames > 0", frames > 0, String(frames));
  }
  // the window server's view: windows and their presents
  const slots = new Map();
  for (const m of err.matchAll(/\[wl-dump\] slot (\d+) id (\d+) kind (\d+) at (-?\d+),(-?\d+) (\d+)x(\d+) .* present (\d+) .*title ("[^"]*")/g)) {
    const e = slots.get(m[1]) || { n: 0, title: m[9], size: m[6] + "x" + m[7], last: 0 };
    e.n++;
    e.last = Math.max(e.last, Number(m[8]));
    slots.set(m[1], e);
  }
  const win = [...slots.entries()].filter(([, e]) => e.last > 0).map(([i, e]) => "slot " + i + " " + e.title + " " + e.size + ": " + e.n + " looks, present " + e.last);
  if (win.length) say("windows", win.join("; "));

  if (prog === "es2gears") {
    check("window drawn", win.length > 0, win.join("; ") || "none");
    const pngs = fs.existsSync(shots) ? fs.readdirSync(shots).filter((f) => /^\d+-\d+\.png$/.test(f)).sort((a, b) => Number(a.split("-")[1].split(".")[0]) - Number(b.split("-")[1].split(".")[0])) : [];
    if (pngs.length) {
      // the last of the first window's frames that has a good probe, else the last
      const results = pngs.map((f) => gearsProbe(path.join(shots, f)));
      const good = results.filter((r) => r.ok).length;
      const last = results[results.length - 1];
      say("probe", good + " of " + results.length + " window PNGs pass; last " + last.text);
      check("pixel probe", good > 0 && last.ok, good + "/" + results.length);
    } else {
      say("probe", "no window PNG in " + shots);
      check("pixel probe", false, "no PNG");
    }
  } else if (prog === "glmark2") {
    const score = out.match(/glmark2 Score: (\d+)/);
    say("glmark2", score ? "Score " + score[1] : "no score line");
    if (score) R.score = Number(score[1]);
    check("glmark2 completed", !!score, score ? score[0] : "no 'glmark2 Score'");
  } else if (prog === "vkr-bench") {
    const rounds = [...out.matchAll(/round \d+:[^\n]*word \d+ ok[^\n]*/g)];
    const stuck = /stuck in fence wait/.test(out);
    const wrong = [...out.matchAll(/round (\d+):[^\n]*word \d+ WRONG/g)].map((m) => m[1]);
    const empty = /empty submit -> fence: [\d.]+ ms each/.test(out);
    // Every fence the guest waited for came back (the rounds, then 100 empty submits) and every round read the word the
    // GPU wrote. A round reading 0 had its fence signalled before the work ran: VK-25 (the first fence of a process,
    // from venus' feedback slot that the blob import overwrote; gpuserver now replies to RESOURCE_CREATE_BLOB only
    // once the executor has taken the record).
    const ok = empty && rounds.length > 0 && !stuck && wrong.length === 0;
    say("vkr-bench", rounds.length + " rounds with the word ok" + (rounds.length ? "; " + rounds[rounds.length - 1][0].slice(0, 150) : ""));
    check("vkr-bench: every fence came back and every round's word is right", ok, ok ? "vkr-bench: ok" :
      stuck ? "stuck in fence wait" : wrong.length ? "round " + wrong.join(",") + " WRONG: the fence signalled before the GPU wrote the word" :
      "no completed rounds / empty-submit line");
  } else if (prog === "vulkaninfo") {
    // `desktop wayland vulkaninfo --summary` with no set-up (VK-08a): the devices the loader found, venus first when the
    // executor has Vulkan; lavapipe (llvmpipe) alone when it has none (--stub). The time is vk-time's, vulkaninfo alone.
    const devs = [...out.matchAll(/deviceName\s*=\s*(.*)/g)].map((m) => m[1].trim());
    const vers = [...out.matchAll(/apiVersion\s*=\s*(\d+)\.(\d+)/g)].map((m) => [Number(m[1]), Number(m[2])]);
    const ms = out.match(/VK_TIME_MS=(\d+)/);
    say("vulkan", devs.length ? devs.join(" | ") : "(no device)");
    if (ms) say("time", "vulkaninfo --summary took " + ms[1] + " ms");
    if (ms) R.ms = Number(ms[1]);
    R.device = devs.join(" | ");
    const venus = devs.findIndex((d) => /Venus/i.test(d));
    const lvp = devs.some((d) => /llvmpipe|lavapipe/i.test(d));
    if (stub) {
      check("no venus under a stub executor (it has no Vulkan)", venus < 0, venus < 0 ? "none" : devs[venus]);
      check("falls to lavapipe", lvp, lvp ? "llvmpipe listed" : "no lavapipe: " + devs.join(" | "));
    } else {
      check("venus device first", venus === 0, venus === 0 ? devs[0] : devs.length ? devs.join(" | ") : "none");
      check("venus is wgvk", venus >= 0 && /wgvk/i.test(devs[venus]), venus >= 0 ? devs[venus] : "none");
      check("Vulkan >= 1.1", venus >= 0 && vers[venus] && (vers[venus][0] > 1 || vers[venus][1] >= 1), venus >= 0 && vers[venus] ? vers[venus].join(".") : "none");
    }
  } else if (prog === "venus-fail") {
    // The venus driver alone against an executor without Vulkan (--stub): the GPU server answers its capset request with
    // "none", the loader has no device and vulkaninfo gives up, in well under 2 s.
    const m = out.match(/VK_TIME_MS=(\d+) rc=(\d+)/);
    say("time", m ? "venus alone took " + m[1] + " ms, rc " + m[2] : "(no time line)");
    check("venus fails, does not hang", !!m && m[2] !== "0" && !/deviceName/.test(out), m ? "rc " + m[2] : "no time line");
    check("venus fails in < 2 s", !!m && Number(m[1]) < 2000, m ? m[1] + " ms" : "no time line");
  } else if (prog === "vkcube") {
    // The device vkcube chose among the loader's (VK-09): venus, the GPU, not lavapipe, whatever order they are listed in.
    const sel = out.match(/Selected GPU (\d+): (.*), type: (\w+)/);
    say("device", sel ? "GPU " + sel[1] + " " + sel[2] + ", type " + sel[3] : "(no 'Selected GPU' line)");
    R.device = sel ? sel[2] : null;
    if (stub) {
      // (VK-17: the executor without Vulkan, as vrend answers: the GPU server offers no venus and vkcube runs on lavapipe, a CPU device, drawing
      // into a window the stub does not show: only the device it chose is checked here)
      check("vkcube fell to lavapipe (no Vulkan in the executor)", !!sel && /llvmpipe|lavapipe/i.test(sel[2]) && sel[3] === "Cpu", sel ? sel[2] + " / " + sel[3] : "no line");
    } else {
    check("vkcube selected the Venus device, a GPU type", !!sel && /Venus/i.test(sel[2]) && /^(Discrete|Integrated)(Gpu)?$/.test(sel[3]), sel ? sel[2] + " / " + sel[3] : "no line");
    check("vkcube presented", s && s.presents > 0, s ? s.presents + " presents" : "no summary");
    }
    const pngs = !stub && fs.existsSync(shots) ? fs.readdirSync(shots).filter((f) => /^gpu-res\d+-\d+\.png$/.test(f)) : [];
    if (stub) {
      // nothing to look at
    } else if (pngs.length) {
      const results = pngs.map((f) => cubeProbe(path.join(shots, f)));
      const good = results.filter((r) => r.ok).length;
      say("probe", good + " of " + results.length + " presented PNGs show the cube; " + results[results.length - 1].text);
      check("pixel probe (the cube, not grey)", good > 0, good + "/" + results.length);
    } else {
      check("pixel probe (the cube, not grey)", false, "no presented PNG");
    }
  }
  else if (prog === "mpv") {
    // mpv --hwdec=vaapi against the stand-in decoder and the real executor (VK-14): the frames the page's decoder side sent, how many the
    // executor drew into the guest's surfaces and by which path, and the picture mpv put in its window.
    const hw = /Using hardware decoding \(vaapi\)/.test(out);
    const stubStats = err.match(/vdec-stub: (\{[^\n]*\})/);
    let ss = null;
    try { ss = stubStats ? JSON.parse(stubStats[1]) : null; } catch (e) { /* below */ }
    const v = s && s.vdec;
    say("mpv", hw ? "hardware decoding (vaapi)" : "NOT hardware decoding: " + (out.match(/Using software decoding[^\n]*/) || ["no 'Using hardware decoding' line"])[0]);
    if (ss) say("decoder", "stand-in: " + ss.chunks + " chunks, " + ss.frames + " frames, " + ss.draws + " sent to the executor, " + ss.errors + " errors");
    if (v) say("executor", "vdecFrames " + v.vdecFrames + ", drawn " + v.vdecDrawn + " (rgba " + v.vdecRgba + ", external " + v.vdecExternal + ", exact " + v.vdecExact
                          + "), skipped " + v.vdecSkipped + ", errors " + v.vdecErrors + "; " + (v.vdecDrawn ? (v.vdecMs / v.vdecDrawn).toFixed(1) + " ms a frame, last " + v.vdecLast : "none drawn")
                          + (v.vdecError ? "; " + v.vdecError : ""));
    R.drawn = v ? v.vdecDrawn : 0;
    if (v && v.vdecDrawn) R.msPerFrame = Number((v.vdecMs / v.vdecDrawn).toFixed(1));
    check("mpv used the VA-API driver", hw, hw ? "yes" : "no");
    check("frames drawn > 0 (rvGpu.stats.exec.vdecDrawn)", !!v && v.vdecDrawn > 0, v ? String(v.vdecDrawn) : "no vdec counters");
    check("no frame skipped or failed", !!v && v.vdecSkipped === 0 && v.vdecErrors === 0, v ? "skipped " + v.vdecSkipped + ", errors " + v.vdecErrors : "no counters");
    check("every frame the decoder sent was received", !!v && !!ss && v.vdecFrames === ss.draws, v && ss ? v.vdecFrames + " of " + ss.draws : "no counters");
    const pngs = fs.existsSync(shots) ? fs.readdirSync(shots).filter((f) => /^gpu-res\d+-\d+\.png$/.test(f)) : [];
    if (pngs.length) {
      const results = pngs.map((f) => cardProbe(path.join(shots, f)));
      const good = results.filter((r) => r.ok).length;
      say("probe", good + " of " + results.length + " window PNGs show the test card; last " + results[results.length - 1].text);
      check("pixel probe (the test card in mpv's window, the right way up)", good > 0, good + "/" + results.length);
    } else {
      check("pixel probe (the test card in mpv's window, the right way up)", false, "no presented PNG");
    }
  }
  else if (prog === "qtquick" || prog === "qtquick-vulkan" || prog === "gtk4" || prog === "chromium") {
    // Apps whose picture is not known in advance (VK-17): the window must be drawn (not one flat colour), the executor's ring must have moved
    // (the global check above) and, for the ones that print a frame rate (the Qt Quick scene, Chromium's page), frames > 0.
    const na = out.match(/^N\/A: .*$/m);
    if (na) { say("n/a", na[0]); check("the program is there", false, na[0]); }
    const info = out.match(/(?:GL_RENDERER|RENDERER:)[^\n]*/);
    if (info) say("gl", info[0].slice(0, 160));
    if (prog === "qtquick-vulkan") {
      // Qt's RHI on Vulkan (VK-27): the instance came up (QT_VULKAN_LIB) and the device it took is venus, not lavapipe or nothing
      const dev = out.match(/qt\.rhi\.general: Physical device \d+: '([^']*)'[^\n]*\n\s*qt\.rhi\.general:\s+using this physical device/);
      if (dev) say("vulkan", dev[1]);
      check("Qt's RHI on Vulkan, on venus", !!dev && /Venus/.test(dev[1]), dev ? dev[1] : (/No QVulkanInstance|Failed to load vulkan/.test(out) ? "no Vulkan instance" : "no 'using this physical device' line"));
    }
    check("window drawn", win.length > 0, win.join("; ") || "no window with a present");
    const pngs = fs.existsSync(shots) ? fs.readdirSync(shots).filter((f) => /\.png$/.test(f) && !/^shot/.test(f)).sort() : [];
    if (pngs.length) {
      const results = pngs.map((f) => drawnProbe(path.join(shots, f)));
      const good = results.filter((r) => r.ok).length;
      const hashes = new Set(results.map((r) => r.hash)).size;
      say("probe", good + " of " + results.length + " window PNGs are drawn (not flat); " + hashes + " different pictures; last " + results[results.length - 1].text);
      check("pixel probe (a drawn window, not a flat colour)", good > 0, good + "/" + results.length);
      if (prog !== "gtk4") check("the picture moves (animated: more than one different PNG)", hashes > 1, hashes + " different");
      R.pictures = hashes;
    } else {
      check("pixel probe (a drawn window, not a flat colour)", false, "no window PNG");
    }
    if (prog !== "gtk4") check("frames reported by the app > 0", fps.length > 0 && fps.reduce((n, m) => n + Number(m[1]), 0) > 0, fps.length ? fps.length + " reports" : "no 'N frames in S seconds' line");
  }
  if (prog === "blender-bg" || prog === "blender") {
    // Blender 4.3 (VK-16). What it printed that says its GL went wrong, and what the executor said about its commands.
    const glerr = [...out.matchAll(/^.*(?:GL(?:SL)? (?:error|compile)|OpenGL error|shader.*(?:error|failed)|Failed to (?:create|compile)|glGetError).*$/gim)].map((m) => m[0].trim());
    const exerr = [...err.matchAll(/^.*(?:ring drain failed|render pipeline not made|device lost|not made).*$/gm)].map((m) => m[0].trim());
    const wgvk = [...err.matchAll(/worker: rvgpu: (wgvk: [^\n]*)/g)].map((m) => m[1]);
    if (wgvk.length) say("wgvk", [...new Set(wgvk)].slice(0, 6).join(" | ").slice(0, 400));
    check("no GL errors in Blender's log", glerr.length === 0, glerr.length ? glerr.slice(0, 3).join(" | ").slice(0, 300) : "none");
    check("the executor made every pipeline and drained its ring", exerr.length === 0, exerr.length ? exerr.slice(0, 3).join(" | ").slice(0, 300) : "none");
  }
  if (prog === "blender-bg") {
    // The Workbench render of the default scene, 480x270, came back as base64 lines ("PNG64 ..."), and is compared with
    // llvmpipe's render of the same (gpu/tools/blender-workbench-480x270.png): at most 0.5% of the pixels more than 8/255 off.
    const b64 = [...out.matchAll(/^PNG64 (\S+)/gm)].map((m) => m[1]).join("");
    if (!b64) {
      say("render", "no PNG came back");
      check("Blender rendered", false, "no PNG64 lines");
    } else {
      const dst = path.join(shots, "blender-render.png");
      fs.mkdirSync(shots, { recursive: true });
      fs.writeFileSync(dst, Buffer.from(b64, "base64"));
      const a = readPng(dst), ref = readPng(path.join(__dirname, "blender-workbench-480x270.png"));
      let bad = 0, max = 0, cube = 0;
      if (a.w === ref.w && a.h === ref.h) {
        for (let i = 0; i < a.w * a.h; i++) {
          let m = 0;
          for (let k = 0; k < 3; k++) m = Math.max(m, Math.abs(a.rgba[i * 4 + k] - ref.rgba[i * 4 + k]));
          if (m > 8) bad++;
          max = Math.max(max, m);
          if (a.rgba[i * 4] > 70) cube++;
        }
      }
      const n = a.w * a.h;
      say("render", a.w + "x" + a.h + ", " + bad + " of " + n + " px differ by more than 8/255 from llvmpipe's (max " + max + "), cube " + (100 * cube / n).toFixed(1) + "% of the frame");
      check("Workbench render matches llvmpipe's (<= 0.5% px > 8/255)", a.w === ref.w && a.h === ref.h && bad <= 0.005 * n && cube > 0.03 * n, bad + " px");
    }
    const t = out.match(/Time: ([\d:.]+)/), w = out.match(/RENDER_WAITED=(\d+)/);
    if (t) say("render time", t[1] + " (Blender's own, in the machine)" + (w ? "; start to \"Blender quit\" " + w[1] + " s" : ""));
  } else if (prog === "blender") {
    // The window server's PNGs of Blender's window: the UI drew (text: light pixels in the header strip), the viewport drew
    // (the grid and the 3D gizmo), more than one frame was presented.
    const pngs = fs.existsSync(shots) ? fs.readdirSync(shots).filter((f) => /^\d+-\d+\.png$/.test(f)).sort((x, y) => Number(x.split("-")[1].split(".")[0]) - Number(y.split("-")[1].split(".")[0])) : [];
    check("Blender's window presented", win.length > 0, win.join("; ") || "none");
    if (pngs.length) {
      const last = readPng(path.join(shots, pngs[pngs.length - 1]));
      let text = 0, colours = new Set();
      for (let y = 0; y < Math.min(60, last.h); y++)
        for (let x = 0; x < last.w; x++) {
          const i = (y * last.w + x) * 4;
          if (last.rgba[i] > 170 && last.rgba[i + 1] > 170 && last.rgba[i + 2] > 170) text++;
        }
      for (let i = 0; i < last.w * last.h; i += 7) colours.add((last.rgba[i * 4] >> 3) << 10 | (last.rgba[i * 4 + 1] >> 3) << 5 | last.rgba[i * 4 + 2] >> 3);
      say("probe", pngs.length + " window PNGs, last " + pngs[pngs.length - 1] + " " + last.w + "x" + last.h + ": " + text + " light text pixels in the top 60 rows, " + colours.size + " distinct colours");
      check("UI text drawn (light pixels in the header strip)", text >= 300, String(text));
      check("more than flat grey (distinct colours)", colours.size >= 40, String(colours.size));
    } else {
      check("window PNGs", false, "none in " + shots);
    }
    const shotsNamed = fs.existsSync(shots) ? fs.readdirSync(shots).filter((f) => !/^\d+-\d+\.png$/.test(f) && /\.png$/.test(f)) : [];
    if (shotsNamed.length) say("shots", shotsNamed.join(" "));
  }
  const prc = out.match(/PROGRAM_RC=(\d+)/);
  if (prc) say("rc", "program " + prc[1] + ", node-machine " + rc);
  else say("rc", "node-machine " + rc + " (no PROGRAM_RC line: the machine did not finish the command)");
  if (!prc && !/^(sh:)/.test(prog)) check("command finished", false, "no PROGRAM_RC");

  console.log("== gpu-nm " + prog + " ==");
  for (const l of lines) console.log(l);
  for (const w of warns) console.log("  WARN " + w);
  for (const c of checks) console.log((c.ok ? "  ok   " : "  FAIL ") + c.name + " (" + c.detail + ")");
  const ok = checks.length > 0 && checks.every((c) => c.ok);
  console.log(ok ? "PASS" : "FAIL");
  R.pass = ok;
  R.failed = checks.filter((c) => !c.ok).map((c) => c.name + " (" + c.detail + ")");
  console.log("MATRIX_RESULT " + JSON.stringify(R));
  process.exit(ok ? 0 : 1);
}

if (require.main === module) main();
module.exports = { readPng, gearsProbe, drawnProbe };
