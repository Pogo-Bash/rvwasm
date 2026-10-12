// Workloads in the manner of Octane/JetStream, small enough to run in the machine in a few seconds each, for
// measuring what the bit-manipulation extensions (Zba, Zbb, Zbs) bring to a JavaScript engine's generated code:
// object graphs and property loads (address arithmetic, pointer decompression), typed arrays (indexing),
// 32-bit integer mixing (rotations, clz32), floating-point objects. Each prints a check value, so that the
// runs with and without the extensions can be seen to compute the same.
//   node zbbench.js [ROUNDS]          every case, ROUNDS times (default 3); the best time of each
//   node zbbench.js CASE [ROUNDS]     one case
//   zbbench.html?ROUNDS               in a browser (the same, the result in the page)
"use strict";

// Richards-like: a scheduler of tasks in linked lists, with packets passed between them.
function richards() {
  class Packet { constructor(link, id, kind) { this.link = link; this.id = id; this.kind = kind; this.a1 = 0; this.a2 = new Array(4).fill(0); } }
  class Task {
    constructor(id, pri, wkq, state) { this.id = id; this.pri = pri; this.wkq = wkq; this.state = state; this.count = 0; this.link = null; }
    run(pkt) {
      this.count++;
      if (pkt) { pkt.a1 = (pkt.a1 + this.id) & 0xffff; pkt.a2[this.count & 3] ^= this.pri; }
      return this.count;
    }
  }
  const tasks = [];
  let packets = null;
  for (let i = 0; i < 64; i++) packets = new Packet(packets, i & 7, i & 1);
  for (let i = 0; i < 16; i++) tasks.push(new Task(i, (i * 37) & 15, null, i & 3));
  let sum = 0;
  for (let round = 0; round < 6000; round++) {
    let p = packets;
    while (p) {
      const t = tasks[(p.id + round) & 15];
      if (t.state !== 3) sum = (sum + t.run(p)) | 0;
      else t.state = round & 3;
      p = p.link;
    }
  }
  for (const t of tasks) sum = (sum + t.count + t.state) | 0;
  return sum;
}

// DeltaBlue-like: a graph of constraint objects whose values are propagated along chains.
function deltablue() {
  class Variable { constructor(v) { this.value = v; this.constraints = []; this.stay = true; this.walk = 0; } }
  class Constraint {
    constructor(a, b, scale, offset) { this.a = a; this.b = b; this.scale = scale; this.offset = offset; a.constraints.push(this); b.constraints.push(this); }
    execute() { this.b.value = this.a.value * this.scale + this.offset; }
  }
  const n = 2000;
  const vars = [];
  for (let i = 0; i < n; i++) vars.push(new Variable(i));
  const cons = [];
  for (let i = 0; i + 1 < n; i++) cons.push(new Constraint(vars[i], vars[i + 1], 1 + (i & 1), i & 3));
  let check = 0;
  for (let round = 0; round < 60; round++) {
    vars[0].value = round;
    for (let i = 0; i < cons.length; i++) cons[i].execute();
    for (let i = 0; i < n; i += 97) check = (check + vars[i].value) % 1000003;
    for (const v of vars) v.walk += v.constraints.length;
  }
  return check;
}

// Typed arrays: a stencil over a Float64Array and a histogram in an Int32Array (indexed loads and stores).
function arrays() {
  const n = 1 << 16;
  const a = new Float64Array(n), b = new Float64Array(n);
  const hist = new Int32Array(256);
  for (let i = 0; i < n; i++) a[i] = (i * 7919) % 1000;
  for (let round = 0; round < 30; round++) {
    for (let i = 1; i < n - 1; i++) b[i] = (a[i - 1] + a[i] + a[i + 1]) / 3;
    for (let i = 0; i < n; i++) hist[b[i] & 255]++;
    const t = a; a.set(b); b.set(t);
  }
  let s = 0;
  for (let i = 0; i < 256; i++) s = (s * 31 + hist[i]) | 0;
  return s;
}

// 32-bit integer work: a SHA-256-style compression over words (rotations, xors), and clz32.
function crypto() {
  const k = new Uint32Array(64);
  for (let i = 0; i < 64; i++) k[i] = (Math.imul(i + 1, 0x9e3779b9) ^ (i << 13)) >>> 0;
  const w = new Uint32Array(64);
  const rotr = (x, n) => (x >>> n) | (x << (32 - n));
  let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a, h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
  let lz = 0;
  for (let block = 0; block < 3000; block++) {
    for (let i = 0; i < 16; i++) w[i] = Math.imul(block + 1, i + 0x01000193) ^ (block << i);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (h + S1 + ch + k[i] + w[i]) | 0;
      const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      lz += Math.clz32(a);
    }
    h0 = (h0 + a) | 0; h1 = (h1 + b) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0;
    h4 = (h4 + e) | 0; h5 = (h5 + f) | 0; h6 = (h6 + g) | 0; h7 = (h7 + h) | 0;
  }
  return ((h0 ^ h3 ^ h7) >>> 0) + lz;
}

// Ray tracing in miniature: small vector objects made and dropped by the million, with floats in them.
function raytrace() {
  class V { constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
    add(o) { return new V(this.x + o.x, this.y + o.y, this.z + o.z); }
    sub(o) { return new V(this.x - o.x, this.y - o.y, this.z - o.z); }
    mul(s) { return new V(this.x * s, this.y * s, this.z * s); }
    dot(o) { return this.x * o.x + this.y * o.y + this.z * o.z; }
    norm() { return this.mul(1 / Math.sqrt(this.dot(this))); } }
  const spheres = [];
  for (let i = 0; i < 8; i++) spheres.push({ c: new V((i % 3) - 1, ((i >> 1) % 3) - 1, 4 + (i & 3)), r: 0.6 + (i & 1) * 0.3 });
  const eye = new V(0, 0, 0), light = new V(1, 1, -1).norm();
  let lit = 0;
  const size = 96;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const dir = new V(x / size - 0.5, y / size - 0.5, 1).norm();
      let best = Infinity, hit = null;
      for (const s of spheres) {
        const oc = eye.sub(s.c), b = oc.dot(dir), c = oc.dot(oc) - s.r * s.r, disc = b * b - c;
        if (disc > 0) { const t = -b - Math.sqrt(disc); if (t > 0 && t < best) { best = t; hit = s; } }
      }
      if (hit) { const p = eye.add(dir.mul(best)), n = p.sub(hit.c).norm(); lit += Math.max(0, n.dot(light)) * 255 | 0; }
    }
  }
  return lit;
}

// Property access on objects of several shapes, a Map, and strings as keys.
function props() {
  const shapes = [];
  for (let i = 0; i < 4000; i++) {
    const o = { id: i, name: "n" + (i % 97) };
    if (i & 1) o.odd = true;
    if (i % 3 === 0) o.third = i / 3;
    if (i % 5 === 0) o.fifth = [i];
    shapes.push(o);
  }
  const m = new Map();
  let s = 0;
  for (let round = 0; round < 40; round++) {
    for (const o of shapes) {
      s = (s + o.id + (o.odd ? 1 : 0) + (o.third | 0) + (o.fifth ? o.fifth[0] : 0)) | 0;
      m.set(o.name, (m.get(o.name) | 0) + 1);
    }
  }
  return s + m.size;
}

const cases = { richards, deltablue, arrays, crypto, raytrace, props };
const now = () => (typeof performance != "undefined" ? performance.now() : Date.now());
const argv = typeof process != "undefined" ? process.argv.slice(2) :
  typeof location != "undefined" && location.search ? location.search.slice(1).split(",") : [];
const only = cases[argv[0]];
const rounds = Number(only ? argv[1] : argv[0]) || 3;
const out = [];
let total = 0;
for (const f of only ? [only] : Object.values(cases)) {
  let best = Infinity, result;
  for (let r = 0; r < rounds; r++) {
    const t = now();
    result = f();
    best = Math.min(best, now() - t);
  }
  total += best;
  out.push(f.name + " " + Math.round(best) + " ms (" + result + ")");
}
const line = "ZBBENCH " + out.join(", ") + "; total " + Math.round(total) + " ms";
console.log(line);
// In a page (zbbench.html): into the document as well, for chromium --headless --dump-dom.
if (typeof document != "undefined") document.body.textContent = line;
