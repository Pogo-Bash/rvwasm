// Run rv's translated regions on a real WebAssembly engine (V8, in node), from a snapshot of the guest's
// state, and time them. This is the performance instrument for the JIT's code generator: the same guest
// state and the same region modules, run by TurboFan (--no-liftoff), by Liftoff (--liftoff-only), or as
// Chrome runs them (the default: Liftoff first, TurboFan when hot).
//
//   node --experimental-wasm-memory64 tools/region-bench.js DUMPDIR SNAPSHOT [max-instructions] [--quiet]
//
// DUMPDIR is where rv wrote its modules (RV_JIT_DUMP=DIR: region-PC.wasm, region-PC.blocks, layout.json),
// SNAPSHOT one of the files RV_JIT_SNAP=DIR wrote (snap-N-PC.bin): the registers and every page of guest
// memory as translated code was about to be entered. The guest's pages are laid out in a memory64 of our
// own, the page table and the dispatcher's map are built as rv builds them (so regions chain to one another
// through the table exactly as in the browser), and the regions run until execution leaves translated code
// (an instruction left to the interpreter, a system call) or the instruction limit is reached. Prints the
// guest instructions run, the time and the time per instruction; with RV_BENCH_JSON=1, one JSON line.
"use strict";
const fs = require("fs");
const path = require("path");

const args = process.argv.slice(2).filter((a) => !a.startsWith("--"));
const quiet = process.argv.includes("--quiet");
// --warmup=N: run N instructions first, then put the snapshot's state back and time a second run: the
// steady state, with every region compiled (and, with dynamic tiering, tiered up) already.
const warmupArg = process.argv.find((a) => a.startsWith("--warmup="));
const warmup = warmupArg ? Number(warmupArg.slice(9)) : 0;
// --settle=MS: after the warm-up, wait that long for V8's background tier-up to finish before timing.
const settleArg = process.argv.find((a) => a.startsWith("--settle="));
const settle = settleArg ? Number(settleArg.slice(9)) : 0;
const [dir, snapPath] = args;
const maxInsns = Number(args[2] || 200e6);
if (!dir || !snapPath) {
  console.error("usage: node --experimental-wasm-memory64 tools/region-bench.js DUMPDIR SNAPSHOT [max-instructions]");
  process.exit(2);
}
const layout = JSON.parse(fs.readFileSync(path.join(dir, "layout.json"), "utf8"));
const PAGE = 4096n;

// The snapshot.
const snap = fs.readFileSync(snapPath);
if (snap.toString("latin1", 0, 8) !== "RVSNAP01") throw new Error("not a snapshot: " + snapPath);
const sv = new DataView(snap.buffer, snap.byteOffset, snap.byteLength);
let off = 8;
const u64 = () => { const v = sv.getBigUint64(off, true); off += 8; return v; };
const u32 = () => { const v = sv.getUint32(off, true); off += 4; return v; };
const S = {};
S.pc = u64(); S.insn_count = u64(); S.region_pc = u64();
S.entry = u32(); S.slot = u32(); S.fcsr = u32(); S.lr_valid = u32();
S.lr_addr = u64(); S.lr_value = u64();
S.x = []; for (let i = 0; i < 32; i++) S.x.push(u64());
S.f = []; for (let i = 0; i < 32; i++) S.f.push(u64());
S.npages = u64(); S.nmapped = u64();
const pages = [];
for (let i = 0n; i < S.nmapped; i++) {
  const page = u64(), flags = u64();
  pages.push({ page, flags, data: snap.subarray(off, off + 4096) });
  off += 4096;
}

// Our memory: [cpu struct][page table][jit struct][map][jit_used] then the guest's pages, each at
// GUEST_BASE + its guest address, so every page's entry has the same distance (GUEST_BASE).
const CPU = 0x10000n;
const TABLE = 0x20000n;                                    // (npages + 1) * 8 bytes
const tableBytes = (S.npages + 1n) * 8n;
const JIT = (TABLE + tableBytes + 0xffffn) & ~0xffffn;     // struct rv_jit stand-in
const regionFiles = fs.readdirSync(dir).filter((f) => /^region-[0-9a-f]+\.wasm$/.test(f));
let nblocks = 0;
for (const f of regionFiles) nblocks += fs.readFileSync(path.join(dir, f.replace(/\.wasm$/, ".blocks")), "utf8").trim().split("\n").length;
let mapBits = 12;
while ((1 << mapBits) < 4 * nblocks && mapBits < 24) mapBits += 2;
const MAP = JIT + 0x10000n;
const mapBytes = BigInt((1 << mapBits) + layout.map_probes - 1) * BigInt(layout.entry_size);
const USED = (MAP + mapBytes + 0xffffn) & ~0xffffn;
const usedBytes = 1n << BigInt(layout.used_bits);
const GUEST_BASE = (USED + usedBytes + 0xfffffn) & ~0xfffffn;
const total = GUEST_BASE + S.npages * PAGE + 0x10000n;
const wasmPages = Number((total + 0xffffn) >> 16n);
const memory = new WebAssembly.Memory({ initial: wasmPages, maximum: wasmPages, shared: true, index: "i64" });
const mem = new Uint8Array(memory.buffer);
const dv = new DataView(memory.buffer);
const w64 = (a, v) => dv.setBigUint64(Number(a), BigInt.asUintN(64, v), true);
const r64 = (a) => dv.getBigUint64(Number(a), true);
const w32 = (a, v) => dv.setUint32(Number(a), Number(v) >>> 0, true);
const r32 = (a) => dv.getUint32(Number(a), true);

// Guest pages and the page table (rv.h: flags in the low 12 bits, the distance above them).
const F = layout;
function restore() {
  for (const p of pages) {
    mem.set(p.data, Number(GUEST_BASE + p.page * PAGE));
    w64(TABLE + p.page * 8n, GUEST_BASE | p.flags);
  }
  for (let i = 0; i < 32; i++) w64(CPU + BigInt(F.x) + BigInt(8 * i), S.x[i]);
  for (let i = 0; i < 32; i++) w64(CPU + BigInt(F.f) + BigInt(8 * i), S.f[i]);
  w64(CPU + BigInt(F.pc), S.pc);
  w32(CPU + BigInt(F.fcsr), S.fcsr);
  w64(CPU + BigInt(F.lr_addr), S.lr_addr);
  w64(CPU + BigInt(F.lr_value), S.lr_value);
  w32(CPU + BigInt(F.lr_valid), S.lr_valid);
  w64(CPU + BigInt(F.insn_count), 0n);
  w64(CPU + BigInt(F.sig_pending), 0n);
  tlbClear();
}
// The translation cache (struct rv_tlb in rv.h: page_r, page_w, delta per register) starts empty, as rv_jit_run() has it.
function tlbClear() {
  if (F.tlb === undefined) return;
  for (let r = 0; r < 32; r++) {
    w64(CPU + BigInt(F.tlb) + BigInt(24 * r), 0x8000000000000000n);
    w64(CPU + BigInt(F.tlb) + BigInt(24 * r + 8), 0x8000000000000000n);
  }
}
restore();

// The cpu's constants.
w64(CPU + BigInt(F.map), TABLE);
w64(CPU + BigInt(F.npages), S.npages);
w64(CPU + BigInt(F.jit), JIT);
w64(CPU + BigInt(F.jit_used), USED);
w64(CPU + BigInt(F.proc), CPU);      // (reserve_gen is read through it: any memory will do)
w64(JIT + BigInt(F.jit_map), MAP);
w32(JIT + BigInt(F.jit_map_bits), mapBits);
// The map's first entries are kept occupied (jit.c: map_reserve()).
for (let i = 0; i < layout.map_probes; i++) {
  w64(MAP + BigInt(i * F.entry_size) + BigInt(F.entry_pc), 1n);
  w32(MAP + BigInt(i * F.entry_size) + BigInt(F.entry_slot), 0xffffffff);
}

const MULT = BigInt("0x" + layout.hash_multiplier);
const hashPc = (pc) => Number((BigInt.asUintN(64, (pc >> 1n) * MULT)) >> BigInt(64 - mapBits));

// Helpers the regions import (jit.c: helpers[]).
const s64 = (v) => BigInt.asIntN(64, v), U = (v) => BigInt.asUintN(64, v);
const s32 = (v) => BigInt.asIntN(32, v), u32v = (v) => BigInt.asUintN(32, v);
const sext32 = (v) => U(BigInt.asIntN(32, v));
let fpCalls = 0;
const helpers = {
  rv_op_mulh: (a, b) => U((s64(a) * s64(b)) >> 64n),
  rv_op_mulhsu: (a, b) => U((s64(a) * U(b)) >> 64n),
  rv_op_mulhu: (a, b) => U((U(a) * U(b)) >> 64n),
  rv_op_div: (a, b) => { a = s64(a); b = s64(b); if (b === 0n) return U(-1n); if (a === -(1n << 63n) && b === -1n) return U(a); return U(a / b); },
  rv_op_divu: (a, b) => { a = U(a); b = U(b); return b === 0n ? U(-1n) : a / b; },
  rv_op_rem: (a, b) => { a = s64(a); b = s64(b); if (b === 0n) return U(a); if (a === -(1n << 63n) && b === -1n) return 0n; return U(a % b); },
  rv_op_remu: (a, b) => { a = U(a); b = U(b); return b === 0n ? a : a % b; },
  rv_op_divw: (a, b) => { a = s32(a); b = s32(b); if (b === 0n) return U(-1n); if (a === -(1n << 31n) && b === -1n) return sext32(a); return sext32(a / b); },
  rv_op_divuw: (a, b) => { a = u32v(a); b = u32v(b); return b === 0n ? U(-1n) : sext32(a / b); },
  rv_op_remw: (a, b) => { a = s32(a); b = s32(b); if (b === 0n) return sext32(a); if (a === -(1n << 31n) && b === -1n) return 0n; return sext32(a % b); },
  rv_op_remuw: (a, b) => { a = u32v(a); b = u32v(b); return sext32(b === 0n ? a : a % b); },
  // The interpreter's floating point: not here. The region is told the instruction failed, and leaves.
  rv_jit_fp: (cpu, insn, a) => { fpCalls++; w32(cpu + BigInt(F.jit_fault), 1); return 0n; },
  rv_jit_csr: (cpu, insn, a) => { fpCalls++; w32(cpu + BigInt(F.jit_fault), 1); return 0n; },
  rv_jit_fused64: (a, b, c) => {      // (not fused: close enough for timing, and the oracle is elsewhere)
    const f = new Float64Array(3); const u = new BigUint64Array(f.buffer);
    u[0] = a; u[1] = b; u[2] = c; f[0] = f[0] * f[1] + f[2]; return u[0];
  },
  rv_jit_reserve: (cpu, addr, how) => 0n,
};

// The regions: each module into a table slot, each block into the map.
const table = new WebAssembly.Table({ initial: 16, element: "anyfunc" });
const byPc = new Map();
let wire = 0;
const t0c = process.hrtime.bigint();
for (const f of regionFiles.sort()) {
  const bytes = fs.readFileSync(path.join(dir, f));
  wire += bytes.length;
  const module = new WebAssembly.Module(bytes);
  const imports = { env: { memory, table }, rv: {} };
  for (const item of WebAssembly.Module.imports(module)) {
    if (item.module === "rv") imports.rv[item.name] = helpers[item.name];
  }
  const instance = new WebAssembly.Instance(module, imports);
  const slot = table.grow(1);
  table.set(slot, instance.exports.r);
  const blocks = fs.readFileSync(path.join(dir, f.replace(/\.wasm$/, ".blocks")), "utf8").trim().split("\n");
  blocks.forEach((line, index) => {
    const pc = BigInt("0x" + line.split(" ")[0]);
    if (byPc.has(pc)) return;            // (another region has the block too: the first keeps it, as rv's map would)
    byPc.set(pc, { slot, index, fn: instance.exports.r });
    let i = hashPc(pc);
    while (r64(MAP + BigInt(i * F.entry_size) + BigInt(F.entry_pc)) !== 0n) i = (i + 1) & ((1 << mapBits) - 1);
    w64(MAP + BigInt(i * F.entry_size) + BigInt(F.entry_pc), pc);
    w32(MAP + BigInt(i * F.entry_size) + BigInt(F.entry_slot), slot);
    w32(MAP + BigInt(i * F.entry_size) + BigInt(F.entry_index), index);
  });
}
const compileMs = Number(process.hrtime.bigint() - t0c) / 1e6;

// Run.
const budget = layout.budget;
let calls = 0, insns = 0n, why = "limit", lastPc = S.pc;
function run(limit) {
  calls = 0; insns = 0n; why = "limit"; lastPc = S.pc;
  for (;;) {
    const pc = r64(CPU + BigInt(F.pc));
    const e = byPc.get(pc);
    if (!e) { why = "left translated code"; lastPc = pc; break; }
    const before = r64(CPU + BigInt(F.insn_count));
    tlbClear();
    e.fn(CPU, e.index, budget);
    calls++;
    const after = r64(CPU + BigInt(F.insn_count));
    if (after === before) { why = fpCalls ? "an instruction left to the interpreter (floating point by the call)" : "an instruction left to the interpreter"; lastPc = pc; break; }
    insns = after;
    if (insns >= BigInt(limit)) break;
  }
}
let warmupMs = 0;
if (warmup) {
  const tw = process.hrtime.bigint();
  run(warmup);
  warmupMs = Number(process.hrtime.bigint() - tw) / 1e6;
  if (settle) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, settle);
  restore();
}
const t0 = process.hrtime.bigint();
run(maxInsns);
const ns = Number(process.hrtime.bigint() - t0);
const chained = F.jit_stat_chained !== undefined ? Number(r64(JIT + BigInt(F.jit_stat_chained))) : -1;
const misses = F.jit_stat_misses !== undefined ? Number(r64(JIT + BigInt(F.jit_stat_misses))) : -1;
const result = {
  snapshot: path.basename(snapPath), regions: regionFiles.length, blocks: nblocks, wire_kib: Math.round(wire / 1024),
  compile_ms: Math.round(compileMs), instructions: Number(insns), calls, ms: Math.round(ns / 1e6),
  ns_per_insn: insns ? (ns / Number(insns)).toFixed(2) : null, stopped: why, at_pc: "0x" + lastPc.toString(16),
  warmup_insns: warmup, warmup_ms: Math.round(warmupMs), chained, misses,
  flags: process.execArgv.filter((a) => a.startsWith("--")).join(" "),
};
if (process.env.RV_BENCH_JSON) console.log(JSON.stringify(result));
else if (!quiet) console.log(`${result.snapshot}: ${result.regions} regions (${result.blocks} blocks, ${result.wire_kib} KiB), compiled in ${result.compile_ms} ms; ` +
  `ran ${result.instructions} guest instructions in ${result.calls} calls (${chained} chained jumps, ${misses} cache misses), ${result.ms} ms = ${result.ns_per_insn} ns/insn; stopped: ${why} at ${result.at_pc}`);
else console.log(`${result.ns_per_insn} ns/insn, ${result.instructions} insns, stopped: ${why}`);
