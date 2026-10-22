#!/usr/bin/env node
"use strict";
// rvgpu-stamp.js: the build stamp of rvgpu.wasm (VK-04). The stamp is a wasm custom section named "rvgpu-stamp"
// appended after the last section (so every wasm consumer ignores it), holding sorted-key JSON:
//   {"branch","commit","dawn","dirty","mesa","patch","stamp":1}
//   branch  the git branch the build came from ("HEAD" when detached)
//   commit  the full commit sha of that checkout ("unknown" when it was not known)
//   mesa    the Mesa pin (gpu/webgpu/build/mesa.pin), dawn the Dawn pin (dawn.pin)
//   patch   sha1 of patches/mesa-page.diff
//   dirty   true when gpu/webgpu or patches had uncommitted changes (or the commit is "unknown")
// Nothing in it is a time or a path, so the stamp does not break byte-reproducible builds.
//
//   node tools/rvgpu-stamp.js FILE.wasm                  print the stamp (exit 1 when there is none)
//   node tools/rvgpu-stamp.js --json FILE.wasm           the raw JSON, one line
//   node tools/rvgpu-stamp.js --require-clean FILE.wasm  print, and exit 2 unless stamped, clean, commit known
//                                                        (the deploy rule: VK-05's release scripts run this)
//   node tools/rvgpu-stamp.js --write FILE.wasm --branch B --commit C --mesa M --dawn D --patch P --dirty true|false
//                                                        replace (or add) the section in place; idempotent
// The writer is gpu/webgpu/build/wasm.sh's last step; the same code reads in a browser: see rvgpu-worker.js.
const fs = require("fs");

const NAME = "rvgpu-stamp";

function leb(n) { const o = []; do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; o.push(b); } while (n); return o; }
function readLeb(b, i) { let v = 0, s = 0, x; do { x = b[i++]; v += (x & 0x7f) * 2 ** s; s += 7; } while (x & 0x80); return [v, i]; }

// [{id, start, end, name}] for each section of a wasm binary; start..end covers id byte, size and payload.
function sections(buf) {
  if (buf.length < 8 || buf.readUInt32LE(0) !== 0x6d736100) throw new Error("not a wasm binary");
  const out = [];
  let i = 8;
  while (i < buf.length) {
    const start = i, id = buf[i++];
    let size; [size, i] = readLeb(buf, i);
    const end = i + size;
    if (end > buf.length) throw new Error("truncated wasm section");
    let name = null;
    if (id === 0) { let l, j; [l, j] = readLeb(buf, i); name = buf.toString("utf8", j, j + l); }
    out.push({ id, start, end, name });
    i = end;
  }
  return out;
}

function encode(stamp) {
  const keys = Object.keys(stamp).sort();
  const sorted = {};
  for (const k of keys) sorted[k] = stamp[k];
  const name = Buffer.from(NAME), body = Buffer.from(JSON.stringify(sorted));
  const payload = Buffer.concat([Buffer.from(leb(name.length)), name, body]);
  return Buffer.concat([Buffer.from([0]), Buffer.from(leb(payload.length)), payload]);
}

function read(buf) {
  for (const s of sections(buf)) {
    if (s.id !== 0 || s.name !== NAME) continue;
    let i = s.start + 1, size, l;
    [size, i] = readLeb(buf, i);
    [l, i] = readLeb(buf, i);
    return JSON.parse(buf.toString("utf8", i + l, s.end));
  }
  return null;
}

function write(buf, stamp) {
  const keep = [buf.subarray(0, 8)];
  for (const s of sections(buf)) if (!(s.id === 0 && s.name === NAME)) keep.push(buf.subarray(s.start, s.end));
  keep.push(encode(stamp));
  return Buffer.concat(keep);
}

// Is this stamp acceptable for a deploy? Returns a list of reasons it is not (empty when fine).
function problems(st) {
  if (!st) return ["unstamped"];
  const p = [];
  if (st.dirty !== false) p.push("dirty build (uncommitted changes, or the commit was not known)");
  if (!/^[0-9a-f]{40}$/.test(st.commit || "")) p.push("no full commit sha");
  return p;
}

module.exports = { NAME, sections, encode, read, write, problems };

if (require.main === module) {
  const a = process.argv.slice(2);
  const opt = (k) => { const i = a.indexOf("--" + k); if (i < 0) return undefined; const [, v] = a.splice(i, 2); return v; };
  const flag = (k) => { const i = a.indexOf("--" + k); if (i < 0) return false; a.splice(i, 1); return true; };
  if (flag("write")) {
    const st = { stamp: 1, branch: opt("branch") || "unknown", commit: opt("commit") || "unknown", mesa: opt("mesa") || "unknown",
                 dawn: opt("dawn") || "unknown", patch: opt("patch") || "unknown", dirty: opt("dirty") !== "false" };
    if (!/^[0-9a-f]{40}$/.test(st.commit)) st.dirty = true;
    const f = a[0];
    fs.writeFileSync(f, write(fs.readFileSync(f), st));
    console.log("rvgpu-stamp: " + f + " " + JSON.stringify(st));
  } else {
    const json = flag("json"), req = flag("require-clean");
    const f = a[0];
    if (!f) { console.error("usage: rvgpu-stamp.js [--json|--require-clean] FILE.wasm"); process.exit(64); }
    const st = read(fs.readFileSync(f));
    if (json) console.log(st ? JSON.stringify(st) : "null");
    else if (!st) console.log(f + ": no rvgpu-stamp section (unstamped build)");
    else {
      console.log(f + ":");
      for (const k of Object.keys(st).sort()) console.log("  " + k.padEnd(7) + " " + st[k]);
    }
    const pr = problems(st);
    if (req && pr.length) { console.error("rvgpu-stamp: refused: " + pr.join("; ")); process.exit(2); }
    if (!st) process.exit(1);
  }
}
