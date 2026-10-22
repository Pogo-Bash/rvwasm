#!/usr/bin/env node
"use strict";
// gears-probe: is es2gears' picture in this PNG (a screenshot of the screen, or of its window)? Counts the pixels of the three
// gears' colours (red 0.8 0.1 0.0, green 0.0 0.8 0.2, blue 0.2 0.2 1.0, lit) as gpu/tools/gpu-nm-report.js does for the window
// alone; here each must be at least MIN percent (default 0.1) of the picture, which for the gears' window in a screen of
// 1024x768 (11% of it) is a few tenths of a percent. A screen the GPU never drew has none of them, or not all three.
//   node tools/gears-probe.js FILE.png [MIN_PERCENT]       exit 0: the three are there
const fs = require("fs");
const zlib = require("zlib");
function readPng(file) {
  const b = fs.readFileSync(file);
  let at = 8, w = 0, h = 0, depth = 0, type = 0;
  const idat = [];
  while (at < b.length) {
    const len = b.readUInt32BE(at), kind = b.toString("ascii", at + 4, at + 8), data = b.subarray(at + 8, at + 8 + len);
    if (kind === "IHDR") { w = data.readUInt32BE(0); h = data.readUInt32BE(4); depth = data[8]; type = data[9]; }
    else if (kind === "IDAT") idat.push(data);
    at += 12 + len;
  }
  if (depth !== 8 || (type !== 6 && type !== 2)) throw new Error("png: only 8-bit RGB/RGBA");
  const bpp = type === 6 ? 4 : 3;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const out = Buffer.alloc(w * h * 4), prev = Buffer.alloc(w * bpp), line = Buffer.alloc(w * bpp);
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
        default: { const p = a + up - c, pa = Math.abs(p - a), pb = Math.abs(p - up), pc = Math.abs(p - c); r = v + (pa <= pb && pa <= pc ? a : pb <= pc ? up : c); }
      }
      line[x] = r & 255;
    }
    line.copy(prev);
    for (let x = 0; x < w; x++) for (let k = 0; k < 3; k++) out[(y * w + x) * 4 + k] = line[x * bpp + k];
  }
  return { w, h, rgba: out };
}
const [file, min = "0.1"] = process.argv.slice(2);
if (!file) { console.error("usage: gears-probe.js FILE.png [MIN_PERCENT]"); process.exit(2); }
const { w, h, rgba } = readPng(file);
let red = 0, green = 0, blue = 0;
for (let i = 0; i < w * h; i++) {
  const r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
  if (r < 24 && g < 24 && b < 24) continue;
  if (r > 60 && r > g * 1.8 && r > b * 1.8) red++;
  else if (g > 60 && g > r * 1.5 && g > b * 1.2) green++;
  else if (b > 60 && b > r * 1.5 && b > g * 1.5) blue++;
}
const pct = (v) => (100 * v / (w * h)).toFixed(2) + "%";
const ok = [red, green, blue].every((v) => 100 * v / (w * h) >= Number(min));
console.log("gears-probe " + file + " " + w + "x" + h + ": red " + pct(red) + ", green " + pct(green) + ", blue " + pct(blue) + (ok ? ": the gears are there" : ": NOT all three gear colours (at least " + min + "% each)"));
process.exit(ok ? 0 : 1);
