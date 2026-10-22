#!/usr/bin/env node
"use strict";
// tools/gpu-matrix.sh's last step: the apps x executors table (markdown) from the runs in a directory.
//   node gpu/tools/gpu-matrix-table.js RUNS_DIR [--md]
// RUNS_DIR/webgpu-APP.txt: what `tools/gpu-nm.sh APP` printed (its report ends in a MATRIX_RESULT line), RUNS_DIR/webgpu-APP.rc its exit status;
// RUNS_DIR/stub-APP.txt: the same under `tools/gpu-nm.sh --stub APP`, the executor that answers like vrend does for Vulkan (no venus): only
// the Vulkan apps have such a cell; the GL apps' vrend cells are `n/a` (vrend is the page's WebGL renderer, there is none under Node).
// RUNS_DIR/meta.json: {commit, branch, date, node, notes}.
const fs = require("fs");
const path = require("path");

const dir = process.argv[2];
if (!dir) { console.error("usage: gpu-matrix-table.js RUNS_DIR"); process.exit(2); }

// the table's rows, in order: [id, what it is]
const APPS = [
  ["es2gears", "es2gears_wayland (GL ES 2, Mesa demo)"],
  ["glmark2", "glmark2-es2-wayland --off-screen (all scenes)"],
  ["qtquick", "a Qt Quick scene (QML, 60 spinning rectangles + text)"],
  ["qtquick-vulkan", "the same scene on Qt's RHI Vulkan backend (QSG_RHI_BACKEND=vulkan, venus)"],
  ["chromium", "Chromium on GPU compositing (cr-drm: RVGPU_DRM=1, ANGLE on Mesa's EGL)"],
  ["gtk4", "a GTK 4 program"],
  ["mpv", "mpv --hwdec=vaapi (the page's video path)"],
  ["vkcube", "vkcube --wsi wayland (Vulkan, venus)"],
  ["vulkaninfo", "vulkaninfo --summary"],
];

function load(prefix, app) {
  const f = path.join(dir, prefix + "-" + app + ".txt");
  if (!fs.existsSync(f)) return null;
  const text = fs.readFileSync(f, "utf8");
  const m = text.match(/^MATRIX_RESULT (\{.*\})$/m);
  let r = null;
  try { r = m ? JSON.parse(m[1]) : null; } catch (e) { /* below */ }
  const rcFile = path.join(dir, prefix + "-" + app + ".rc");
  const na = text.match(/^N\/A: (.*)$/m);
  return { text, r, rc: fs.existsSync(rcFile) ? Number(fs.readFileSync(rcFile, "utf8")) : null, na: na ? na[1] : null,
           wall: fs.existsSync(path.join(dir, prefix + "-" + app + ".wall")) ? Number(fs.readFileSync(path.join(dir, prefix + "-" + app + ".wall"), "utf8")) : null };
}

function cell(run) {
  if (!run) return "not run";
  if (run.na) return "n/a: " + run.na;
  const r = run.r;
  if (!r) return "FAIL: no report (rc " + run.rc + ")";
  const bits = [];
  if (r.frames !== undefined) bits.push(r.frames + " frames" + (r.fps ? ", " + r.fps + " fps" : ""));
  if (r.score !== undefined) bits.push("Score " + r.score);
  if (r.presents) bits.push(r.presents + " presents");
  if (r.drawn !== undefined) bits.push(r.drawn + " video frames drawn" + (r.msPerFrame ? " (" + r.msPerFrame + " ms)" : ""));
  if (r.ms !== undefined) bits.push((r.ms / 1000).toFixed(1) + " s");
  if (r.device) bits.push(r.device.replace(/\s*\(.*$/, ""));
  if (r.submits) bits.push(r.submits + " submits");
  const head = r.pass ? "PASS" : "FAIL";
  const why = r.pass ? "" : " [" + (r.failed || []).join("; ").replace(/\|/g, "/") + "]";
  return head + ": " + bits.join(", ") + why;
}

const meta = fs.existsSync(path.join(dir, "meta.json")) ? JSON.parse(fs.readFileSync(path.join(dir, "meta.json"), "utf8")) : {};
const stamps = new Set();
let rows = [];
let pass = 0, fail = 0, na = 0;
for (const [id, what] of APPS) {
  const w = load("webgpu", id);
  const s = load("stub", id);
  if (w && w.r && w.r.stamp) stamps.add(w.r.stamp);
  let vr;
  if (s && !s.na) vr = "stub executor, no Vulkan (as vrend answers): " + cell(s);
  else if (w && w.na) vr = "n/a: " + w.na;
  else vr = "n/a: vrend is WebGL in the page, not runnable under Node; the owner's run (VK-17 sheet)";
  const wc = cell(w);
  if (wc.startsWith("PASS")) pass++; else if (wc.startsWith("n/a")) na++; else fail++;
  rows.push("| " + id + " | " + what + " | " + wc.replace(/\|/g, "/") + " | " + vr.replace(/\|/g, "/") + " |");
}
const out = [];
out.push("| app | what | webgpu (nm+dawn, the real executor) | vrend |");
out.push("| --- | --- | --- | --- |");
out.push(...rows);
out.push("");
out.push("webgpu column: " + pass + " PASS, " + fail + " FAIL, " + na + " n/a of " + APPS.length + " apps. rvgpu stamp: " + ([...stamps].join(", ") || "(none)") +
         (meta.commit ? "; tree " + meta.branch + " " + meta.commit : "") + (meta.date ? "; run " + meta.date : "") + ".");
console.log(out.join("\n"));
