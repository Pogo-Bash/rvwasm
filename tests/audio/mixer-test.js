#!/usr/bin/env node
// The page's player (tools/www/audio-worklet.js: RvAudioMixer) against a header laid out as the kernel lays it out
// (sound/drivers/wasm_audio.c), without a machine: formats, resampling, the count of what was taken (the hardware
// pointer), period interrupts, silence past what the program wrote, the master volume.
//   node tests/audio/mixer-test.js
"use strict";
const path = require("path");
const { RvAudioMixer } = require(path.join(__dirname, "../../tools/www/audio-worklet.js"));

let failures = 0;
const check = (what, ok, detail) => {
  console.log((ok ? "ok   " : "FAIL ") + what + (detail ? ": " + detail : ""));
  if (!ok) failures++;
};

const HEADER = 4096, IRQ_WORD = 8192, RING = 65536;
const make = () => {
  const buffer = new SharedArrayBuffer(1 << 20);
  const h32 = new Int32Array(buffer, HEADER, 256), h64 = new BigUint64Array(buffer, HEADER, 128);
  h32[0] = 0x55415652; h32[1] = 1; h32[2] = 4; h32[3] = 128; h32[4] = 64;
  h32[5] = 127; h32[6] = 127; h32[7] = -6350; h32[8] = 50; h32[9] = 1; h32[10] = 4;
  return { buffer, h32, h64 };
};
// Stream i's words: 32-bit index and 64-bit index of a field at byte offset `off` in its block.
const w32 = (i, off) => (64 + i * 128 + off) / 4, w64 = (i, off) => (64 + i * 128 + off) / 8;
const start = (m, i, { frames, period, rate, channels, format, ring }) => {
  const { h32, h64 } = m;
  h64[w64(i, 8)] = BigInt(ring);
  h32[w32(i, 16)] = frames; h32[w32(i, 20)] = period; h32[w32(i, 24)] = rate; h32[w32(i, 28)] = channels;
  h32[w32(i, 32)] = format;
  h64[w64(i, 40)] = h64[w64(i, 64)];	// base = consumed
  h64[w64(i, 48)] = BigInt(Number.MAX_SAFE_INTEGER);	// valid_end: everything
  h32[w32(i, 0)]++;
  h32[w32(i, 4)] = 1;
};
const position = (m, i) => Number(m.h64[w64(i, 64)] - m.h64[w64(i, 40)]);
// Frequency by zero crossings (rising).
const frequency = (x, rate) => {
  let n = 0, first = -1, last = -1;
  for (let k = 1; k < x.length; k++) {
    if (x[k - 1] < 0 && x[k] >= 0) {
      if (first < 0) first = k; else n++;
      last = k;
    }
  }
  return n ? n * rate / (last - first) : 0;
};
const rms = (x) => Math.sqrt(x.reduce((a, v) => a + v * v, 0) / x.length);
const run = (mixer, blocks) => {
  const L = new Float32Array(blocks * 128), R = new Float32Array(blocks * 128);
  const l = new Float32Array(128), r = new Float32Array(128);
  for (let b = 0; b < blocks; b++) {
    mixer.render(l, r, 128);
    L.set(l, b * 128);
    R.set(r, b * 128);
  }
  return { L, R };
};

// 1. S16 stereo at 44100 into 48000: 1 kHz left, 500 Hz right; the pointer advances 44100 per 48000 played.
{
  const m = make();
  const frames = 8192, rate = 44100;
  const ring = new Int16Array(m.buffer, RING, frames * 2);
  for (let k = 0; k < frames; k++) {
    ring[2 * k] = Math.round(16000 * Math.sin(2 * Math.PI * 1000 * k / rate));
    ring[2 * k + 1] = Math.round(16000 * Math.sin(2 * Math.PI * 500 * k / rate));
  }
  start(m, 0, { frames, period: 1024, rate, channels: 2, format: 2, ring: RING });
  const mixer = new RvAudioMixer(m.buffer, HEADER, IRQ_WORD, 48000);
  const blocks = 375;	// 1 s at 48000
  const { L, R } = run(mixer, blocks);
  const fl = frequency(L.subarray(4800), 48000), fr = frequency(R.subarray(4800), 48000);
  check("S16 44100->48000 left is 1 kHz", Math.abs(fl - 1000) < 2, fl.toFixed(2) + " Hz");
  check("S16 44100->48000 right is 500 Hz", Math.abs(fr - 500) < 2, fr.toFixed(2) + " Hz");
  check("amplitude kept", Math.abs(rms(L.subarray(4800)) - 16000 / 32768 / Math.SQRT2) < 0.01, rms(L.subarray(4800)).toFixed(4));
  const pos = position(m, 0);
  check("pointer advanced by 44100 per second played (+3 lookahead)", Math.abs(pos - 44100) <= 3, String(pos));
  const irqs = m.h32[w32(0, 76)];
  check("one interrupt per period crossed", Math.abs(irqs - Math.floor(pos / 1024)) <= 1, irqs + " for " + pos + " frames");
  const word = new BigInt64Array(m.buffer, IRQ_WORD, 1)[0];
  check("interrupt 4 raised in the IRQ word", word === 16n, String(word));
  check("heartbeat counts blocks", m.h32[13] === blocks, String(m.h32[13]));
  check("no underruns", m.h32[w32(0, 72)] === 0, String(m.h32[w32(0, 72)]));
}

// 2. FLOAT mono at 48000: no resampling, exact; then valid_end: silence past it, but the pointer runs on.
{
  const m = make();
  const frames = 4800;
  const ring = new Float32Array(m.buffer, RING, frames);
  for (let k = 0; k < frames; k++) ring[k] = 0.5 * Math.sin(2 * Math.PI * 440 * k / 48000);
  start(m, 1, { frames, period: 480, rate: 48000, channels: 1, format: 14, ring: RING });
  m.h64[w64(1, 48)] = 2400n;	// the program wrote 2400 frames only
  const mixer = new RvAudioMixer(m.buffer, HEADER, 0, 48000);
  const { L, R } = run(mixer, 30);	// 3840 frames
  let exact = true;
  for (let k = 128; k < 2400; k++) {	// (the first block fades in from the player's start)
    if (Math.abs(L[k] - ring[k]) > 1e-6 || Math.abs(R[k] - ring[k]) > 1e-6) { exact = false; break; }
  }
  check("FLOAT mono 48000: samples exact, on both channels", exact);
  check("silence past valid_end", rms(L.subarray(2500)) === 0, rms(L.subarray(2500)).toFixed(6));
  check("pointer runs on past valid_end", position(m, 1) === 30 * 128 + 2, String(position(m, 1)));
  check("underruns counted", m.h32[w32(1, 72)] > 0, String(m.h32[w32(1, 72)]));
}

// 3. U8, S24 and S32 levels; volume and mute; two substreams mixed; a stopped one is left alone.
{
  const m = make();
  const u8 = new Uint8Array(m.buffer, RING, 1000).fill(192);	// +0.5
  const s32 = new Int32Array(m.buffer, RING + 4096, 2000).fill(-1073741824);	// -0.5
  start(m, 0, { frames: 1000, period: 100, rate: 48000, channels: 1, format: 1, ring: RING });
  start(m, 2, { frames: 1000, period: 100, rate: 48000, channels: 2, format: 10, ring: RING + 4096 });
  const mixer = new RvAudioMixer(m.buffer, HEADER, 0, 48000);
  let { L } = run(mixer, 4);
  check("U8 + S32 mixed to 0", Math.abs(L[300]) < 1e-6, String(L[300]));
  m.h32[w32(2, 4)] = 0;	// stop the S32 one
  const before = position(m, 2);
  ({ L } = run(mixer, 4));
  check("stopped substream not taken from", position(m, 2) === before, String(position(m, 2)));
  check("U8 alone is +0.5", Math.abs(L[300] - 0.5) < 1e-6, String(L[300]));
  m.h32[5] = 127 - 12;	// -6 dB
  ({ L } = run(mixer, 4));
  check("volume -6 dB", Math.abs(L[300] - 0.5 * Math.pow(10, -6 / 20)) < 1e-4, String(L[300]));
  m.h32[9] = 0;
  ({ L } = run(mixer, 4));
  check("mute", L[300] === 0, String(L[300]));
  new Int32Array(m.buffer, RING + 4096, 2000).fill(0x00400000);	// S24 +0.5 in the low 24 bits
  m.h32[w32(2, 32)] = 6;
  m.h32[w32(2, 0)]++;
  m.h32[w32(2, 4)] = 1;
  m.h32[9] = 1; m.h32[5] = 127;
  m.h32[w32(0, 4)] = 0;
  ({ L } = run(mixer, 4));
  check("S24 +0.5", Math.abs(L[300] - 0.5) < 1e-6, String(L[300]));
  void s32; void u8;
}

// 4. Upsampling 8000 -> 48000 keeps the tone and runs the pointer at 8000/s.
{
  const m = make();
  const frames = 800, ring = new Int16Array(m.buffer, RING, frames);
  for (let k = 0; k < frames; k++) ring[k] = Math.round(20000 * Math.sin(2 * Math.PI * 200 * k / 8000));
  start(m, 3, { frames, period: 80, rate: 8000, channels: 1, format: 2, ring: RING });
  const mixer = new RvAudioMixer(m.buffer, HEADER, 0, 48000);
  const { L } = run(mixer, 375);
  const f = frequency(L.subarray(4800), 48000);
  check("8000->48000 200 Hz", Math.abs(f - 200) < 1, f.toFixed(2) + " Hz");
  check("pointer at 8000 per second", Math.abs(position(m, 3) - 8000) <= 4, String(position(m, 3)));
}

console.log(failures ? failures + " failed" : "all passed");
process.exit(failures ? 1 : 0);
