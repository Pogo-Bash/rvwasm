// The executor Worker's side of a GPU window (stage B) under Node: tools/www/gpu-exec.js as the Worker runs it, with
// the page's messages and no WebGL (a context whose every call is a no-op, the small `proto` executor on it).
// What the page does with a window whose slot says format 2 (display.js, gpu-exec.js on the page): it tells the
// Worker where the window server's table is ("table"), and asks for a resource's picture ("grab"); the Worker
// watches the table and sends a "present" (an ImageBitmap) for every present whose gpu_serial the ring's done has
// passed, and one for every grab. Without those answers the page draws nothing of the window and never
// acknowledges its presents, so the client's buffers are never released: mpv in the release stood in
// eglSwapBuffers with its four colour buffers busy and no window to show for it (the two handlers had been lost
// when the video decoder's port was added, 661c4e0).
//
//   node gpu/tools/exec-worker-test.js                     (Node 22 and later)
//   GPUADDR_HIGH_GIB=4.5 node gpu/tools/exec-worker-test.js     the same with the ring, the table and the resource's memory
//                                                              at 4.5 GiB and up in a 64-bit memory (Node 26, or Node 22 with
//                                                              --experimental-wasm-memory64): the machine's addresses on a
//                                                              computer with ?mem=8192; `>> 2` on them goes negative from
//                                                              2 GiB and wraps to a low address from 4 GiB (2.5 and 4.5 are the cases)
"use strict";
// (An address of the machine's memory as the index of its 32-bit word: exact for addresses past 2 and 4 GiB, which `>> 2` is not.)
const ix32 = (a) => Math.floor(a / 4);

const fs = require("fs");
const path = require("path");
const vm = require("vm");

// ---- a Worker's globals, and a WebGL2 context that does nothing
const messages = [];
globalThis.self = globalThis;
self.postMessage = (m) => messages.push(m);
const constant = (name) => [...name].reduce((h, c) => (h * 31 + c.charCodeAt(0)) & 0xffff, 7);
const gl = new Proxy({}, {
  get(_, name) {
    if (typeof name !== "string") {
      return undefined;
    }
    if (/^[A-Z][A-Z_0-9]*$/.test(name)) {
      return constant(name);
    }
    return () => ({ gl: name });
  },
});
globalThis.OffscreenCanvas = class {
  constructor(w, h) {
    this.width = w;
    this.height = h;
  }
  getContext() {
    return gl;
  }
  transferToImageBitmap() {
    return { close() {} };
  }
};
globalThis.window = undefined;

vm.runInThisContext(fs.readFileSync(path.join(__dirname, "../../tools/www/gpu-exec.js"), "utf8"), { filename: "gpu-exec.js" });
if (typeof self.onmessage !== "function") {
  throw new Error("gpu-exec.js did not take the Worker's side (self.onmessage)");
}

// ---- the machine's memory: a ring with one RES_CREATE record, and a surface table with one GPU window
const HIGH = Math.floor(Number(process.env.GPUADDR_HIGH_GIB || 0) * 1073741824);
const memory = (() => {
  if (!HIGH) {
    return new WebAssembly.Memory({ initial: 64, maximum: 64, shared: true });
  }
  const pages = BigInt(Math.ceil((HIGH + 0x400000) / 65536));
  try {
    return new WebAssembly.Memory({ address: "i64", initial: pages, maximum: pages, shared: true });	// (the standard's form: Node 26)
  } catch (error) {
    return new WebAssembly.Memory({ index: "i64", initial: Number(pages), maximum: Number(pages), shared: true });	// (the older form: Node 22 with the flag)
  }
})();
const w32 = new Int32Array(memory.buffer);
const ring = HIGH + 0x10000, dataOff = 4096, dataSize = 65536;
const H_VERSION = 4, H_PID = 8, H_DATA_SIZE = 16, H_DATA_OFF = 20, H_HEAD = 24, H_DOORBELL = 28, H_TAIL = 64, H_DONE = 68;
w32[ix32(ring + 0)] = 0x50475652;
w32[ix32(ring + H_VERSION)] = 2;
w32[ix32(ring + H_PID)] = 190;
w32[ix32(ring + H_DATA_SIZE)] = dataSize;
w32[ix32(ring + H_DATA_OFF)] = dataOff;
const RES = 25, PID = 190;
// (res, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags, size, addr lo, hi)
const backing = HIGH + 0x200000;
const record = [RES, 2, 1, 0x10, 640, 360, 1, 1, 0, 0, 0, 640 * 360 * 4, backing % 0x100000000, Math.floor(backing / 0x100000000)];
let p = ix32(ring + dataOff);
w32[p++] = 3;	// OP_RES_CREATE
w32[p++] = 1;	// ctx
w32[p++] = 7;	// serial
w32[p++] = record.length * 4;
for (const x of record) {
  w32[p++] = x;
}
w32[ix32(ring + H_HEAD)] = 16 + ((record.length * 4 + 15) & ~15);
w32[ix32(ring + H_TAIL)] = 0;

const table = HIGH + 0x80000, header = 2048, slotSize = 384, slots = 64;
const S_ID = 4, S_PRESENT = 56, S_BUFFER_LO = 60, S_BUFFER_HI = 64, S_FORMAT = 72, S_GPU_SERIAL = 244;
const slot = table + header;	// slot 0
w32[ix32(slot + S_ID)] = 4;
w32[ix32(slot + S_BUFFER_LO)] = RES;
w32[ix32(slot + S_BUFFER_HI)] = PID;
w32[ix32(slot + S_FORMAT)] = 2;
const present = (id, gpuSerial) => {
  w32[ix32(slot + S_PRESENT)] = id;
  w32[ix32(slot + S_GPU_SERIAL)] = gpuSerial;
};
present(1, 7);	// (done reaches 7 once the record is run)

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const presents = () => messages.filter((m) => m.type === "present");
const fail = (what) => {
  console.error("exec-worker-test: FAILED: " + what + "\n  messages: " + JSON.stringify(messages.map((m) => m.type === "present"
    ? { type: m.type, res: m.res, frame: m.frame, grabbed: m.grabbed } : m.type)));
  process.exit(1);
};

(async () => {
  self.onmessage({ data: { type: "start", memory, ring, exec: "proto" } });
  await sleep(30);
  if (!messages.some((m) => m.type === "ready")) {
    fail("no 'ready' after start");
  }
  if ((Atomics.load(w32, ix32(ring + H_DONE)) >>> 0) !== 7) {
    fail("the ring's done is " + w32[ix32(ring + H_DONE)] + ", not 7: the RES_CREATE record was not run");
  }
  // 1. The table: the Worker finds the window's present by itself (watchTable, every 2 ms).
  self.onmessage({ data: { type: "table", address: table, slots, slotSize, header, gpuPid: PID } });
  await sleep(40);
  let got = presents();
  if (got.length !== 1 || got[0].res !== RES || got[0].frame !== 1 || !got[0].grabbed) {
    fail("expected one present of resource " + RES + " for present 1 from the table");
  }
  // The same present is not sent twice.
  await sleep(20);
  if (presents().length !== 1) {
    fail("present 1 was sent again");
  }
  // 2. A newer present whose gpu_serial done has not reached: not sent; then a record of that serial is run
  // (done passes it): sent.
  present(2, 9);
  await sleep(20);
  if (presents().length !== 1) {
    fail("present 2 (gpu_serial 9, done 7) was sent before the GPU was done with it");
  }
  p = ix32(ring + dataOff + w32[ix32(ring + H_HEAD)]);	// (after the first record's padding)
  w32[p++] = 4;	// OP_RES_UNREF of a resource there is not: a record that does nothing
  w32[p++] = 1;
  w32[p++] = 9;
  w32[p++] = 16;
  w32[p++] = 999;
  w32[p++] = 0;
  w32[p++] = 0;
  w32[p++] = 0;
  Atomics.store(w32, ix32(ring + H_HEAD), w32[ix32(ring + H_HEAD)] + 32);
  Atomics.add(w32, ix32(ring + H_DOORBELL), 1);
  Atomics.notify(w32, ix32(ring + H_DOORBELL));
  await sleep(100);
  if ((Atomics.load(w32, ix32(ring + H_DONE)) >>> 0) !== 9) {
    fail("the ring's done is " + w32[ix32(ring + H_DONE)] + ", not 9");
  }
  await sleep(20);
  got = presents();
  if (got.length !== 2 || got[1].frame !== 2) {
    fail("present 2 was not sent once done passed its gpu_serial");
  }
  // 3. A grab from the page (display.js's texture() asking): answered at once with that present's number.
  self.onmessage({ data: { type: "grab", ctx: 0, res: RES, present: 3 } });
  await sleep(5);
  got = presents();
  if (got.length !== 3 || got[2].frame !== 3 || got[2].res !== RES || !got[2].grabbed) {
    fail("the grab of resource " + RES + " for present 3 was not answered");
  }
  // 4. A hidden page: nothing from the table; shown again: the newest present comes.
  self.onmessage({ data: { type: "hidden", hidden: true } });
  present(4, 9);
  await sleep(20);
  if (presents().length !== 3) {
    fail("a present was sent while the page is hidden");
  }
  self.onmessage({ data: { type: "hidden", hidden: false } });
  await sleep(20);
  if (presents().length !== 4 || presents()[3].frame !== 4) {
    fail("present 4 was not sent once the page was shown again");
  }
  console.log("exec-worker-test: ok (a GPU window's presents from the table: 1, 2 after done, 4 after hidden; a grab: 3)");
  process.exit(0);
})().catch((e) => fail(String(e && e.stack || e)));
