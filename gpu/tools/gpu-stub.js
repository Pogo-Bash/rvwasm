// A stand-in for the page's executor under tools/node-machine.js (--hook gpu/tools/gpu-stub.js): it finds
// gpuserver's ring (BroadcastChannel "rv-gpu"), hands out caps recorded from a real virglrenderer
// (gpu/tools/caps-gles3.0.bin: virgl_test_server on a GLES 3.0 host, as gpu/tools/vtest-caps.py took
// them), reads every record, and completes it at once. It draws nothing: a TRANSFER_GET fills the box in
// the backing with a colour that changes with every frame, so that what reaches a window shows that the
// whole way (client, socket, gpuserver, ring, backing, client's copy, wl_shm, wlserver) works.
//
//   GPU_STUB_CHAIN=tools/wl-dump.js  also load that hook (node-machine takes one)
//   GPU_STUB_CAPS=FILE               other caps (made like the page's vrend's: GPU_STUB_CAPS_RAW=1 not to)
//   GPU_STUB_LAG=MS                  complete records only this long after they came (a slow executor)
//   GPU_STUB_DIE=S                   stop moving after S seconds (the executor gone)
"use strict";

const fs = require("fs");
const path = require("path");

module.exports = (os, opts) => {
  const chained = process.env.GPU_STUB_CHAIN ? require(path.resolve(process.env.GPU_STUB_CHAIN))(os, opts) : null;
  const capsFile = process.env.GPU_STUB_CAPS || path.join(__dirname, "caps-gles3.0.bin");
  const capsData = Buffer.from(fs.readFileSync(capsFile));
  // As the page's vrend gives them (gpu/vrend/rvgpu.c rvg_caps): no TRANSFER, no COPY_TRANSFER, and no copy
  // transfers from the host (capability_bits at 392, capability_bits_v2 at 688). GPU_STUB_CAPS_RAW=1: the
  // recorded caps as they are (with those bits; Mesa then never takes the paths the browser takes).
  if (!process.env.GPU_STUB_CAPS_RAW && capsData.length >= 692) {
    capsData.writeUInt32LE((capsData.readUInt32LE(392) & ~((1 << 17) | (1 << 26))) >>> 0, 392);
    capsData.writeUInt32LE((capsData.readUInt32LE(688) & ~(1 << 7)) >>> 0, 688);
  }
  // The recorded caps are virglrenderer 1.0.0's, whose host_feature_check_version (offset 556 in
  // struct virgl_caps_v2) is 19; Mesa then drops to vtest protocol 2, whose resource handles are the
  // client's own and collide between clients. The executor's virglrenderer (1.3.0, main) says 23, and so
  // does this stand-in for it.
  if (capsData.length >= 560 && capsData.readUInt32LE(556) < 23) {
    capsData.writeUInt32LE(23, 556);
  }
  const lag = Number(process.env.GPU_STUB_LAG || 0);
  const dieAfter = Number(process.env.GPU_STUB_DIE || 0);
  const t0 = Date.now();

  const R = { CTX_DONE: 1024, HEAD: 24, DOORBELL: 28, SUBMITTED: 32, TAIL: 64, DONE: 68, EXEC_HEARTBEAT: 72, EXEC_STATE: 76, CAPS_LEN: 84 };
  const NAMES = ["PAD", "CTX_CREATE", "CTX_DESTROY", "RES_CREATE", "RES_UNREF", "SUBMIT", "TRANSFER_PUT", "TRANSFER_GET", "BLOB_CREATE"];
  const counts = {};
  const resources = new Map();
  const contexts = new Map();
  let ring = 0, dataSize = 0, dataOff = 0, frame = 0, submitBytes = 0, lastSerial = 0;
  const pending = [];	// [time, serial]

  const channel = new BroadcastChannel("rv-gpu");
  channel.onmessage = (e) => {
    const m = e.data;
    if (!m || m.type !== "gpu" || ring) {
      return;
    }
    ring = m.ring;
    const v = new DataView(os.memory.buffer);
    if (v.getUint32(ring, true) !== 0x50475652) {
      console.error("gpu-stub: no ring at " + ring);
      ring = 0;
      return;
    }
    dataSize = v.getUint32(ring + 16, true);
    dataOff = v.getUint32(ring + 20, true);
    new Uint8Array(os.memory.buffer).set(capsData, m.caps);
    v.setUint32(ring + R.CAPS_LEN, capsData.length, true);
    Atomics.store(new Int32Array(os.memory.buffer), (ring + R.EXEC_STATE) >> 2, 1);
    console.error("gpu-stub: ring at " + ring + " (pid " + m.pid + "), " + capsData.length + " bytes of caps given");
  };

  const fillBox = (r, a) => {
    // a: res level x y z w h d offset data_size
    const bpp = 4;
    const stride = r.width * bpp;
    const m = new Uint8Array(os.memory.buffer);
    const colour = [(frame * 37) & 255, (frame * 91) & 255, 200, 255];	// B G R A
    for (let y = 0; y < a[6]; y++) {
      const row = r.address + a[8] + y * stride;
      if (row + a[5] * bpp > r.address + r.size) {
        break;
      }
      for (let x = 0; x < a[5]; x++) {
        m.set(colour, row + x * bpp);
      }
    }
    frame++;
  };

  const turn = () => {
    if (!ring) {
      return;
    }
    if (dieAfter && Date.now() - t0 > dieAfter * 1000) {
      return;
    }
    const w = new Int32Array(os.memory.buffer);
    const v = new DataView(os.memory.buffer);
    Atomics.add(w, (ring + R.EXEC_HEARTBEAT) >> 2, 1);
    const head = Atomics.load(w, (ring + R.HEAD) >> 2) >>> 0;
    let tail = Atomics.load(w, (ring + R.TAIL) >> 2) >>> 0;
    while (tail !== head) {
      const at = ring + dataOff + (tail % dataSize);
      const op = v.getUint32(at, true), ctx = v.getUint32(at + 4, true), serial = v.getUint32(at + 8, true);
      const bytes = v.getUint32(at + 12, true);
      const a = [];
      for (let i = 0; i < Math.min(bytes / 4, 16); i++) {
        a.push(v.getUint32(at + 16 + 4 * i, true));
      }
      counts[NAMES[op] || op] = (counts[NAMES[op] || op] || 0) + 1;
      if (op === 1) {
        const name = Buffer.from(new Uint8Array(os.memory.buffer, at + 16, bytes)).toString().replace(/\0.*$/s, "");
        contexts.set(ctx, name);
        console.error("gpu-stub: context " + ctx + " (" + name + ")");
      } else if (op === 3) {
        resources.set(a[0], { width: a[4], height: a[5], format: a[2], bind: a[3], size: a[11], address: a[12] + a[13] * 0x100000000 });
        if (process.env.GPU_STUB_VERBOSE) {
          console.error("gpu-stub: res " + a[0] + " " + a[4] + "x" + a[5] + " format " + a[2] + " bind 0x" + a[3].toString(16) + " size " + a[11]);
        }
      } else if (op === 4) {
        resources.delete(a[0]);
      } else if (op === 5) {
        submitBytes += bytes;
      } else if (op === 7) {
        const r = resources.get(a[0]);
        if (r && (r.bind & (1 << 17)) && r.size >= 16) {
          // A query's buffer (Mesa's virgl_query: PIPE_BIND_CUSTOM, struct virgl_host_query_state): done, as the
          // page's vrend says once WebGL has the result (Mesa reads it again and again until it is).
          const w = new DataView(os.memory.buffer, r.address, 16);
          w.setUint32(0, 1, true);	// VIRGL_QUERY_STATE_DONE
          w.setUint32(4, 8, true);
          w.setBigUint64(8, 1000n, true);
        } else if (r) {
          fillBox(r, a);
        }
      }
      if (op !== 0) {
        lastSerial = serial;
        pending.push([Date.now(), serial, ctx]);
      }
      tail = (tail + 16 + ((bytes + 15) & ~15)) >>> 0;
    }
    Atomics.store(w, (ring + R.TAIL) >> 2, tail | 0);
    // In order: the context's done word, then the global one, then wake whoever waits on the global one.
    let done = -1;
    while (pending.length && Date.now() - pending[0][0] >= lag) {
      const [, serial, ctx] = pending.shift();
      if (ctx > 0 && ctx < 256) {
        Atomics.store(w, (ring + R.CTX_DONE + 4 * ctx) >> 2, serial | 0);
      }
      done = serial;
    }
    if (done >= 0) {
      Atomics.store(w, (ring + R.DONE) >> 2, done | 0);
      Atomics.notify(w, (ring + R.DONE) >> 2);
    }
  };
  const timer = setInterval(turn, 1);

  return {
    done() {
      clearInterval(timer);
      channel.close();
      console.error("gpu-stub: records " + JSON.stringify(counts) + ", " + (submitBytes / 1024).toFixed(0) + " KiB of commands, "
          + frame + " transfers back, contexts " + JSON.stringify([...contexts.values()]));
      return chained && chained.done ? chained.done() : undefined;
    },
  };
};
