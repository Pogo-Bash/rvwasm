// A stand-in for gpuserver and a guest GL program behind it (gpu-demo.html starts it as a Worker): writes into
// the executor's ring (gpu/PROTOCOL.md in branch gpu-guest, "the ring", version 2) what gpuserver would pass on
// for a program drawing spinning triangles with Mesa's virgl driver: resources with their backing in shared
// memory (two colour buffers to draw into in turn, a vertex buffer filled by TRANSFER_PUT), shaders as TGSI
// text, and per frame a clear, per triangle its constants and a draw, then the frame shown one of two ways:
//
//   stage B (default): the frame's resource is handed to the page (as wlserver would put it into the window's
//     slot from a dmabuf: format 2, the resource, gpu_serial = `submitted` after the frame); the page draws the
//     executor's texture of it once `done` has passed that serial;
//   stage A (readback: 1): what stock Mesa's vtest winsys does (PROTOCOL.md, presenting): TRANSFER_GET of the
//     whole frame into the backing, wait for `done`, copy it into a wl_shm-like buffer, and have the page
//     upload that like any software-drawn window.
//
// One frame is outstanding at a time: the next is made when the page has drawn the last (frame callbacks).
//
// Message in: { memory, ring, times, frameWords, backing, wlshm, width, height, draws, readback }.
// times: 64 doubles, the time each frame was submitted (performance.timeOrigin + now, by frame % 64), for the
// page to measure the latency to the screen. frameWords: four words, [frame ready, its resource, its
// gpu_serial, frame the page drew] (stage A: the frame is in wlshm; resource and serial unused).
"use strict";

const H_MAGIC = 0, H_VERSION = 4, H_PID = 8, H_DATA_SIZE = 16, H_DATA_OFF = 20, H_HEAD = 24, H_DOORBELL = 28,
      H_SUBMITTED = 32, H_TAIL = 64, H_DONE = 68;
const OP_PAD = 0, OP_CTX_CREATE = 1, OP_RES_CREATE = 3, OP_SUBMIT = 5, OP_TRANSFER_PUT = 6, OP_TRANSFER_GET = 7;
const CCMD_CREATE_OBJECT = 1, CCMD_BIND_OBJECT = 2, CCMD_SET_VIEWPORT_STATE = 4, CCMD_SET_FRAMEBUFFER_STATE = 5,
      CCMD_SET_VERTEX_BUFFERS = 6, CCMD_CLEAR = 7, CCMD_DRAW_VBO = 8, CCMD_SET_CONSTANT_BUFFER = 12,
      CCMD_BIND_SHADER = 31;
const OBJ_BLEND = 1, OBJ_RASTERIZER = 2, OBJ_DSA = 3, OBJ_SHADER = 4, OBJ_VERTEX_ELEMENTS = 5, OBJ_SURFACE = 8;
const FORMAT_R8G8B8A8 = 67, FORMAT_R32G32_FLOAT = 29, FORMAT_R32G32B32_FLOAT = 30;
const BIND_RENDER_TARGET = 2, BIND_SAMPLER_VIEW = 8, BIND_VERTEX_BUFFER = 16;
const TARGET_BUFFER = 0, TARGET_2D = 2, PRIM_TRIANGLES = 4, CLEAR_COLOR0 = 4;
const CTX = 1;

const VS = `VERT
DCL IN[0]
DCL IN[1]
DCL OUT[0], POSITION
DCL OUT[1], GENERIC[0]
DCL CONST[0][0..3]
DCL TEMP[0]
  0: MUL TEMP[0], CONST[0][0], IN[0].xxxx
  1: MAD TEMP[0], CONST[0][1], IN[0].yyyy, TEMP[0]
  2: MAD TEMP[0], CONST[0][2], IN[0].zzzz, TEMP[0]
  3: MAD OUT[0], CONST[0][3], IN[0].wwww, TEMP[0]
  4: MOV OUT[1], IN[1]
  5: END
`;
const FS = `FRAG
DCL IN[0], GENERIC[0], PERSPECTIVE
DCL OUT[0], COLOR
  0: MOV OUT[0], IN[0]
  1: END
`;

let w32, ring, size, data, serial = 0;

const cmd0 = (cmd, obj, len) => (cmd | (obj << 8) | (len << 16)) >>> 0;
const f32tmp = new Float32Array(1), u32tmp = new Uint32Array(f32tmp.buffer);
const fbits = (x) => {
  f32tmp[0] = x;
  return u32tmp[0];
};
const textWords = (text) => {
  const b = new TextEncoder().encode(text + "\0");
  const padded = new Uint8Array((b.length + 3) & ~3);
  padded.set(b);
  return Array.from(new Uint32Array(padded.buffer));
};

// One record into the ring: {op, ctx, serial, bytes} + payload, 16-byte aligned, never wrapping. Returns
// its serial. Waits for room.
const put = (op, words, completion) => {
  const bytes = words.length * 4;
  const need = 16 + ((bytes + 15) & ~15);
  for (;;) {
    const head = Atomics.load(w32, (ring + H_HEAD) >> 2) >>> 0, tail = Atomics.load(w32, (ring + H_TAIL) >> 2) >>> 0;
    let at = head % size, skip = 0;
    if (at + need > size) {
      skip = size - at;
    }
    if ((head - tail) + skip + need <= size) {
      if (skip) {
        const q = (data + at) >> 2;
        w32[q] = OP_PAD;
        w32[q + 1] = 0;
        w32[q + 2] = 0;
        w32[q + 3] = skip - 16;
      }
      if (skip) {
        at = 0;
      }
      const p = (data + at) >> 2;
      serial++;
      w32[p] = op;
      w32[p + 1] = CTX;
      w32[p + 2] = serial;
      w32[p + 3] = bytes;
      for (let i = 0; i < words.length; i++) {
        w32[p + 4 + i] = words[i];
      }
      if (completion) {
        Atomics.store(w32, (ring + H_SUBMITTED) >> 2, serial);
      }
      Atomics.store(w32, (ring + H_HEAD) >> 2, (head + skip + need) | 0);
      Atomics.add(w32, (ring + H_DOORBELL) >> 2, 1);
      Atomics.notify(w32, (ring + H_DOORBELL) >> 2);
      return serial;
    }
    Atomics.wait(w32, (ring + H_TAIL) >> 2, tail | 0, 5);
  }
};

const waitDone = (s) => {
  for (;;) {
    const done = Atomics.load(w32, (ring + H_DONE) >> 2) >>> 0;
    if (done >= s) {
      return;
    }
    Atomics.wait(w32, (ring + H_DONE) >> 2, done | 0, 100);
  }
};

self.onmessage = (event) => {
  const m = event.data;
  const mem = m.memory;
  ring = m.ring;
  w32 = new Int32Array(mem.buffer);
  const f64 = new Float64Array(mem.buffer), u8 = new Uint8Array(mem.buffer);
  const W = m.width, H = m.height, draws = m.draws || 1;

  // The ring's header, as gpuserver makes it.
  size = 4 << 20;
  w32[(ring + H_VERSION) >> 2] = 2;
  w32[(ring + H_PID) >> 2] = m.pid;
  w32[(ring + H_DATA_SIZE) >> 2] = size;
  w32[(ring + H_DATA_OFF) >> 2] = 2048;
  data = ring + 2048;
  Atomics.store(w32, (ring + H_MAGIC) >> 2, 0x50475652);

  put(OP_CTX_CREATE, textWords("gpu-stub"));
  // Resources (ids are gpuserver's, global): 1, 2 the colour buffers; 3 the vertex buffer (x, y, r, g, b).
  let backing = m.backing;
  const res = (id, target, format, bind, w, h, bytes) => {
    const addr = backing;
    backing += (bytes + 4095) & ~4095;
    put(OP_RES_CREATE, [id, target, format, bind, w, h, 1, 1, 0, 0, 0, bytes, addr, 0]);
    return addr;
  };
  res(1, TARGET_2D, FORMAT_R8G8B8A8, BIND_RENDER_TARGET | BIND_SAMPLER_VIEW, W, H, W * H * 4);
  const back2 = res(2, TARGET_2D, FORMAT_R8G8B8A8, BIND_RENDER_TARGET | BIND_SAMPLER_VIEW, W, H, W * H * 4);
  const back1 = back2 - ((W * H * 4 + 4095) & ~4095);
  const verts = [0, 0.6, 1, 0.2, 0.2, -0.52, -0.3, 0.2, 1, 0.2, 0.52, -0.3, 0.2, 0.4, 1];
  const vaddr = res(3, TARGET_BUFFER, 0, BIND_VERTEX_BUFFER, verts.length * 4, 1, verts.length * 4);
  new Float32Array(mem.buffer, vaddr, verts.length).set(verts);
  put(OP_TRANSFER_PUT, [3, 0, 0, 0, 0, verts.length * 4, 1, 1, 0, verts.length * 4], true);

  // Objects: shaders 10, 11; vertex elements 12; surfaces 13, 14 (on resources 1, 2); blend 15 (all colours
  // written), rasterizer 16 (depth clip, half-pixel centres, counter-clockwise front), depth-stencil 17 (off),
  // as Mesa's virgl driver binds them before drawing. A shader's length counts its NUL, and num_tokens is
  // what vrend sizes its token array by (Mesa sends 300 for small shaders).
  const vs = textWords(VS), fs = textWords(FS);
  put(OP_SUBMIT, [
    cmd0(CCMD_CREATE_OBJECT, OBJ_SHADER, 5 + vs.length), 10, 0, VS.length + 1, 300, 0, ...vs,
    cmd0(CCMD_CREATE_OBJECT, OBJ_SHADER, 5 + fs.length), 11, 1, FS.length + 1, 300, 0, ...fs,
    cmd0(CCMD_CREATE_OBJECT, OBJ_BLEND, 11), 15, 0, 0, (0xf << 27) >>> 0, 0, 0, 0, 0, 0, 0, 0,
    cmd0(CCMD_CREATE_OBJECT, OBJ_RASTERIZER, 9), 16, ((1 << 1) | (1 << 15) | (1 << 29)) >>> 0, fbits(1), 0, 0, fbits(1), 0, 0, 0,
    cmd0(CCMD_CREATE_OBJECT, OBJ_DSA, 5), 17, 0, 0, 0, 0,
    cmd0(CCMD_BIND_OBJECT, OBJ_BLEND, 1), 15,
    cmd0(CCMD_BIND_OBJECT, OBJ_RASTERIZER, 1), 16,
    cmd0(CCMD_BIND_OBJECT, OBJ_DSA, 1), 17,
    cmd0(CCMD_CREATE_OBJECT, OBJ_VERTEX_ELEMENTS, 9), 12, 0, 0, 0, FORMAT_R32G32_FLOAT, 8, 0, 0, FORMAT_R32G32B32_FLOAT,
    cmd0(CCMD_CREATE_OBJECT, OBJ_SURFACE, 5), 13, 1, FORMAT_R8G8B8A8, 0, 0,
    cmd0(CCMD_CREATE_OBJECT, OBJ_SURFACE, 5), 14, 2, FORMAT_R8G8B8A8, 0, 0,
  ], true);

  // Frames.
  let frame = 0;
  const t0 = performance.now();
  const fw = m.frameWords >> 2;
  for (;;) {
    frame++;
    const back = frame & 1 ? 1 : 2;
    const surface = back === 1 ? 13 : 14;
    const t = (performance.now() - t0) / 1000;
    const words = [
      cmd0(CCMD_SET_FRAMEBUFFER_STATE, 0, 3), 1, 0, surface,
      cmd0(CCMD_SET_VIEWPORT_STATE, 0, 7), 0, fbits(W / 2), fbits(H / 2), fbits(0.5), fbits(W / 2), fbits(H / 2), fbits(0.5),
      cmd0(CCMD_CLEAR, 0, 8), CLEAR_COLOR0, fbits(0.08), fbits(0.1), fbits(0.14), fbits(1), 0, 0, 0,
      cmd0(CCMD_BIND_SHADER, 0, 2), 10, 0,
      cmd0(CCMD_BIND_SHADER, 0, 2), 11, 1,
      cmd0(CCMD_BIND_OBJECT, OBJ_VERTEX_ELEMENTS, 1), 12,
      cmd0(CCMD_SET_VERTEX_BUFFERS, 0, 3), 20, 0, 3,
    ];
    // Triangles on a grid, each turning: a column-major matrix of scale s, angle a, at (x, y), aspect kept.
    const cols = Math.ceil(Math.sqrt(draws));
    const s = draws === 1 ? 1 : 1.6 / cols;
    for (let i = 0; i < draws; i++) {
      const a = t * 1.5 + i * 0.37;
      const x = draws === 1 ? 0 : -1 + (i % cols + 0.5) * 2 / cols, y = draws === 1 ? 0 : 1 - (Math.floor(i / cols) + 0.5) * 2 / cols;
      const c = Math.cos(a) * s, sn = Math.sin(a) * s, k = H / W;
      words.push(cmd0(CCMD_SET_CONSTANT_BUFFER, 0, 18), 0, 0,
        fbits(c * k), fbits(sn), 0, 0,
        fbits(-sn * k), fbits(c), 0, 0,
        0, 0, fbits(1), 0,
        fbits(x), fbits(y), 0, fbits(1));
      words.push(cmd0(CCMD_DRAW_VBO, 0, 12), 0, 3, PRIM_TRIANGLES, 0, 1, 0, 0, 0, 0, 0, 2, 0);
    }
    f64[(m.times >> 3) + (frame % 64)] = performance.timeOrigin + performance.now();
    const submitted = put(OP_SUBMIT, words, true);
    if (m.readback) {
      // Stage A: the frame back into its backing, then into the window's own buffer, then the page uploads it.
      const g = put(OP_TRANSFER_GET, [back, 0, 0, 0, 0, W, H, 1, 0, W * H * 4], true);
      waitDone(g);
      u8.copyWithin(m.wlshm, back === 1 ? back1 : back2, (back === 1 ? back1 : back2) + W * H * 4);
    } else {
      w32[fw + 1] = back;
      w32[fw + 2] = submitted;
    }
    Atomics.store(w32, fw, frame);
    for (;;) {
      const shown = Atomics.load(w32, fw + 3);
      if (shown >= frame) {
        break;
      }
      Atomics.wait(w32, fw + 3, shown, 100);
    }
  }
};
