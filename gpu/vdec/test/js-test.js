// gpuserver's vdec.js and the page's vdec-worker.js core together, in plain Node: a stand-in for gpuserver's
// helpers (memory files are pieces of one shared memory), a client that says what the guest's driver says, and a
// stand-in VideoDecoder. Checks the records, the status words the driver waits on, copies (NV12 and I420) and
// draws (zero copy), errors and the key chunk rule.
//   node gpu/vdec/test/js-test.js
"use strict";
const fs = require("fs");
const path = require("path");
const assert = require("assert");
const { VdecCore } = require("../../../tools/www/vdec-worker.js");

const memory = new WebAssembly.Memory({ initial: 512, maximum: 512, shared: true });	// 32 MiB
let brk = 1 << 20;
const alloc = (n) => {
  const at = brk;
  brk += (n + 4095) & ~4095;
  return at;
};
const replies = [];
const gs = {
  dv: () => new DataView(memory.buffer), u8: () => new Uint8Array(memory.buffer), i32: () => new Int32Array(memory.buffer),
  log: (t) => process.env.V && console.log(t), alloc, pid: 42,
  backing: (size) => ({ address: alloc(size), size, fd: 99, dev: 1, ino: brk }),
  letGo: () => {}, sent: () => {},
  sendFd: () => true,
  reply: (client, cmd, list) => (replies.push({ cmd, list }), true),
  disconnect: (client, why) => { throw new Error("disconnect: " + why); },
};
const vdec = (0, eval)(fs.readFileSync(path.join(__dirname, "../../server/vdec.js"), "utf8"))(gs);

// ---- a stand-in VideoDecoder: every chunk a frame, unless it is VP9's 1-byte show_existing_frame... no: all of
// them; frames of chunks nobody waits for are closed by the core.
let failNext = false, format = "NV12";
class Frame {
  constructor(ts, w, h) {
    Object.assign(this, { timestamp: ts, codedWidth: w, codedHeight: h, displayWidth: w, displayHeight: h,
      visibleRect: { x: 0, y: 0, width: w, height: h }, format, closed: false });
  }
  clone() { return new Frame(this.timestamp, this.codedWidth, this.codedHeight); }
  close() { this.closed = true; }
  async copyTo(dest, { rect, layout }) {
    const v = this.timestamp & 255;
    if (this.format === "NV12") {
      for (let y = 0; y < rect.height; y++) dest.fill(v, layout[0].offset + y * layout[0].stride, layout[0].offset + y * layout[0].stride + rect.width);
      for (let y = 0; y < rect.height / 2; y++) dest.fill(200, layout[1].offset + y * layout[1].stride, layout[1].offset + y * layout[1].stride + rect.width);
    } else {
      dest.fill(v, 0, rect.width * rect.height);
      dest.fill(10, rect.width * rect.height, rect.width * rect.height * 5 / 4);
      dest.fill(20, rect.width * rect.height * 5 / 4, rect.width * rect.height * 3 / 2);
    }
    return layout;
  }
}
const decoders = [];
class VideoDecoder {
  constructor({ output, error }) { Object.assign(this, { output, error, state: "unconfigured", chunks: [] }); decoders.push(this); }
  configure(c) { this.config = c; this.state = "configured"; }
  decode(chunk) {
    this.chunks.push(chunk);
    if (failNext) {
      failNext = false;
      setImmediate(() => { this.state = "closed"; this.error(new Error("EncodingError: stand-in")); });
      return;
    }
    const f = new Frame(chunk.timestamp, this.config.codedWidth, this.config.codedHeight);
    setImmediate(() => this.state === "configured" && this.output(f));
  }
  close() { this.state = "closed"; }
}
class EncodedVideoChunk {
  constructor({ type, timestamp, data }) { Object.assign(this, { type, timestamp, data: new Uint8Array(data) }); }
}
const draws = [];
const gpuPort = { postMessage(m) { draws.push(m); m.frame.close(); Atomics.store(new Int32Array(memory.buffer), m.ready >> 2, m.seq); } };
const core = new VdecCore({ memory, ring: vdec.ring, VideoDecoder, EncodedVideoChunk, gpuPort, log: () => {} });

// ---- the guest's side
const client = { name: "test", fd: 7 };
const say = (cmd, ...words) => {
  const at = alloc(4 * words.length + 4);
  words.forEach((w, i) => new DataView(memory.buffer).setUint32(at + 4 * i, w >>> 0, true));
  return vdec.command(client, cmd, at);
};
const C = { HELLO: 0x10010, ALLOC: 0x10011, FREE: 0x10012, STREAM: 0x10013, STREAM_END: 0x10014, SURFACE: 0x10015,
  DECODE: 0x10016, COPY: 0x10017, EXPORT: 0x10018, SLOT_END: 0x10019 };
const tick = () => new Promise((r) => setImmediate(r));
const turn = async () => { vdec.turn(performance.now()); core.turn(); await tick(); await tick(); core.turn(); };
const status = () => vdec.clients.get(client).status.address;
const word = (off) => new DataView(memory.buffer).getUint32(status() + off, true);
const slotWord = (slot, i) => word(4096 + 32 * slot + 4 * i);

(async () => {
  // HELLO before the page: "not yet"
  say(C.HELLO, 1);
  assert.deepStrictEqual(replies.pop().list, [1, 0], "HELLO before the page is ready");
  core.publish([{ codec: 1, profile: 0, max_w: 3840, max_h: 2160, flags: 1 }, { codec: 4, profile: 0, max_w: 3840, max_h: 2160, flags: 1 }], true);
  vdec.turn(performance.now());
  assert.strictEqual(say(C.HELLO, 1), true);
  const hello = replies.pop().list;
  assert.deepStrictEqual(hello.slice(0, 2), [0, 2], "HELLO: ok, 2 codecs");
  assert.strictEqual(hello[2 + 4] & 4, 4, "caps[0] says zero copy");
  assert.strictEqual(word(0), 0x53565652, "status file magic");
  // a chunk area and an image
  say(C.ALLOC, 1 << 20);
  const [mem] = replies.pop().list;
  say(C.ALLOC, 1984 * 1088 * 3 / 2);
  const [imem] = replies.pop().list;
  await turn();
  const chunkAddr = [...vdec.clients.get(client).mems.entries()].find(([id]) => id === mem)[1].address;
  const imgAddr = [...vdec.clients.get(client).mems.entries()].find(([id]) => id === imem)[1].address;
  new Uint8Array(memory.buffer, chunkAddr, 8).set([0, 0, 0, 1, 0x65, 1, 2, 3]);
  // a stream; a delta chunk first is refused (WebCodecs needs a key chunk)
  say(C.STREAM, 1, 1, 100, 40, 1920, 1088, 0);
  say(C.DECODE, 1, 5, 1, mem, 0, 8, 0, 8);
  await turn();
  assert.strictEqual(slotWord(5, 4), 1, "a delta chunk first: error");
  assert.strictEqual(word(12), 8, "consumed");
  assert.strictEqual(decoders[0].config.codec, "avc1.640028");
  assert.strictEqual(decoders[0].config.hardwareAcceleration, "prefer-hardware");
  // key chunk: decoded
  say(C.DECODE, 1, 5, 2, mem, 8, 8, 1, 16);
  await turn();
  assert.strictEqual(slotWord(5, 0), 2, "decoded");
  assert.strictEqual(decoders[0].chunks[0].type, "key");
  // a copy of it
  say(C.COPY, 5, 77, 2, imem, 0, 1984, 1984 * 1088, 1984, 1920, 1088, 1984 * 1088 * 3 / 2);
  await turn();
  await tick();
  assert.strictEqual(slotWord(5, 2), 77, "copied");
  assert.strictEqual(slotWord(5, 3), 0, "copy ok");
  assert.strictEqual(new Uint8Array(memory.buffer)[imgAddr + 1984 * 10 + 5], 2, "luma written");
  assert.strictEqual(new Uint8Array(memory.buffer)[imgAddr + 1984 * 1088 + 1984 * 3 + 7], 200, "chroma written");
  // a copy asked before its frame is there waits for it
  say(C.DECODE, 1, 6, 3, mem, 16, 8, 0, 24);
  say(C.COPY, 6, 78, 3, imem, 0, 1984, 1984 * 1088, 1984, 1920, 1088, 1984 * 1088 * 3 / 2);
  await turn();
  await tick();
  assert.strictEqual(slotWord(6, 2), 78, "a copy that waited for its frame");
  // I420 frames are interleaved into NV12
  format = "I420";
  say(C.DECODE, 1, 7, 4, mem, 24, 8, 0, 32);
  await turn();
  say(C.COPY, 7, 79, 4, imem, 0, 1984, 1984 * 1088, 1984, 1920, 1088, 1984 * 1088 * 3 / 2);
  await turn();
  await tick();
  format = "NV12";
  const u8 = new Uint8Array(memory.buffer);
  assert.strictEqual(slotWord(7, 2), 79);
  assert.deepStrictEqual([u8[imgAddr + 1984 * 1088], u8[imgAddr + 1984 * 1088 + 1]], [10, 20], "I420 -> NV12");
  // zero copy: SURFACE + EXPORT draws the frame it has, then every decode into it is drawn
  say(C.SURFACE, 7, 1920, 1088, 11, 12);
  say(C.EXPORT, 7);
  await turn();
  assert.strictEqual(draws.length, 1, "export drew the frame there");
  assert.deepStrictEqual(draws[0].res, [11, 12]);
  assert.strictEqual(slotWord(7, 1), 4, "ready");
  say(C.DECODE, 1, 7, 5, mem, 32, 8, 4, 40);
  await turn();
  assert.strictEqual(draws.length, 2);
  assert.strictEqual(slotWord(7, 1), 5, "drawn when decoded");
  // hidden chunks give no wait
  say(C.DECODE, 1, 8, 6, mem, 40, 8, 2, 48);
  await turn();
  assert.strictEqual(slotWord(8, 0), 0, "hidden: not a frame for its slot");
  // a decoder error: the pending chunk fails, a new decoder waits for a key chunk
  failNext = true;
  say(C.DECODE, 1, 9, 7, mem, 48, 8, 0, 56);
  await turn();
  await tick();
  assert.strictEqual(slotWord(9, 4), 7, "decoder error -> error word");
  say(C.DECODE, 1, 9, 8, mem, 56, 8, 0, 64);
  await turn();
  assert.strictEqual(slotWord(9, 4), 8, "after an error a delta chunk fails");
  say(C.DECODE, 1, 9, 9, mem, 64, 8, 1, 72);
  await turn();
  assert.strictEqual(slotWord(9, 0), 9, "a key chunk after the error decodes");
  // the end
  say(C.SLOT_END, 9);
  say(C.STREAM_END, 1);
  say(C.FREE, mem);
  vdec.gone(client);
  await turn();
  assert.strictEqual(core.clients.size, 0, "client closed on the page");
  console.log("js-test: all ok (" + JSON.stringify(core.stats) + ")");
})().catch((e) => {
  console.error("js-test: FAIL", e);
  process.exit(1);
});
