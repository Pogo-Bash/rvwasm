// The page's GPU executor (prototype): runs guest programs' GPU command streams on WebGL2, and gives the
// window server's page side (display.js) the textures that result, to draw as windows without their pixels
// passing through the CPU. gpu/FEEDBACK.md (this branch) and gpu/PROTOCOL.md (branch gpu-guest) say where this
// is going; this file is the host's half of the prototype.
//
// The stream comes through gpuserver's ring (PROTOCOL.md, "the ring": version 1), in the machine's shared
// memory: records {op, ctx, serial, bytes} + payload, written by gpuserver (or, in gpu-demo.html, by a stub
// Worker), consumed here. SUBMIT records carry virgl's own command stream (virgl_protocol.h: a word
// cmd | object << 8 | length << 16, then the words). Only what a triangle needs is decoded here, and shaders,
// which come as TGSI text, are put into GLSL ES 3.00 by a few lines that know the handful of instructions the
// stub's shaders use. The real executor is to be virglrenderer's vrend built to wasm (its GLES path, on this
// same kind of context); what is meant to stay from this file is around it: the ring and its completion
// rules, transfers straight between the machine's memory and the GPU, the present, and the two ways of handing
// a texture to display.js:
//
//   ?gpu=main    this runs on the page's main thread, in display.js's own WebGL2 context: a guest window's
//                texture is the resource's texture itself, drawn as it is (no copy at all).
//   ?gpu=worker  this runs in a Worker, on a WebGL2 context of its own (an OffscreenCanvas); a present blits
//                the resource into that canvas, takes it as an ImageBitmap (transferToImageBitmap) and posts
//                it to the page, where it goes into the window's texture (a copy, but from GPU memory to GPU
//                memory: no pixel passes through the CPU). The page's main thread does no GL work for it but
//                that one texImage2D.
//
// Completion (PROTOCOL.md): `done` rises in order. SUBMIT and TRANSFER_PUT are done when they have been
// handed to WebGL (WebGL keeps their order with everything after them); TRANSFER_GET when its bytes are in the
// backing (readPixels into a pixel-pack buffer, a fence polled with timeout 0 on later turns, then
// getBufferSubData straight into the shared memory: WebGL cannot wait for the GPU); PRESENT when the page has
// drawn it (window.rvGpu.shown()).
//
// On the page: window.rvGpu. texture(pid, res, present) -> { texture, flipY, bgra, w, h, present } or null
// (display.js asks it for slots of format 2: buffer_lo = res, buffer_hi = gpuserver's pid); done(); onPresent (set by whoever puts presents
// into slots: (ctx, res, frame, serial)); shown(serial) (a PRESENT record has been drawn); start(memory, ring,
// gl) (also done by itself on gpuserver's announcement on BroadcastChannel "rv-gpu"); stats.
(() => {
  "use strict";

  // The index of the 32-bit word at a byte address of the machine's memory. That memory can be larger than 4 GiB
  // (?mem=8192, the default on a big computer), so an address is an exact Number of up to 2^34 and more: never
  // `>> 2`, `& ~n`, `| 0` or `>>> 0` on one (32-bit operators: from 2 GiB the index goes negative, from 4 GiB it
  // wraps to a LOW address, the kernel's).
  const ix32 = (address) => Math.floor(address / 4);
  const RING_MAGIC = 0x50475652;	// "RVGP"
  const H_MAGIC = 0, H_VERSION = 4, H_PID = 8, H_DATA_SIZE = 16, H_DATA_OFF = 20, H_HEAD = 24, H_DOORBELL = 28,
        H_TAIL = 64, H_DONE = 68, H_EXEC_HEARTBEAT = 72, H_EXEC_STATE = 76, H_CAPS_LEN = 84, H_CTX_DONE = 1024;
  const OP_PAD = 0, OP_CTX_CREATE = 1, OP_CTX_DESTROY = 2, OP_RES_CREATE = 3, OP_RES_UNREF = 4, OP_SUBMIT = 5,
        OP_TRANSFER_PUT = 6, OP_TRANSFER_GET = 7, OP_PRESENT = 9, OP_RES_ATTACH = 12, OP_RES_DETACH = 13;
  const CCMD_CREATE_OBJECT = 1, CCMD_BIND_OBJECT = 2, CCMD_DESTROY_OBJECT = 3, CCMD_SET_VIEWPORT_STATE = 4,
        CCMD_SET_FRAMEBUFFER_STATE = 5, CCMD_SET_VERTEX_BUFFERS = 6, CCMD_CLEAR = 7, CCMD_DRAW_VBO = 8,
        CCMD_RESOURCE_INLINE_WRITE = 9, CCMD_SET_CONSTANT_BUFFER = 12, CCMD_SET_SUB_CTX = 28,
        CCMD_CREATE_SUB_CTX = 29, CCMD_BIND_SHADER = 31;	// (checked against virgl_protocol.h with a C compiler)
  const OBJ_SHADER = 4, OBJ_VERTEX_ELEMENTS = 5, OBJ_SURFACE = 8;
  const SHADER_VERTEX = 0, SHADER_FRAGMENT = 1;
  const TARGET_BUFFER = 0;
  const FORMAT_R32G32_FLOAT = 29, FORMAT_R32G32B32_FLOAT = 30, FORMAT_R32G32B32A32_FLOAT = 31;
  const PRIMS = [0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006];	// points .. triangle fan: GL's own numbers
  const CLEAR_COLOR0 = 4;


  const inWorker = typeof document === "undefined";
  const now = () => performance.timeOrigin + performance.now();

  // ---- which executor (VK-13). The page asks this once, when gpuserver announces its ring (page side below).
  // Precedence: ?exec= in the address > rv-site.json "gpu_exec" (the server's switch: docs/deploy.md "GPU executor
  // switch") > vrend. The default is vrend and stays it unless the server says otherwise AND this browser passes:
  //   site vrend (or missing, or anything else)  -> vrend
  //   site webgpu  -> webgpu if navigator.gpu exists and the adapter probe passes, else vrend
  //   site auto    -> the same, and only in Chrome or Edge on a desktop (decision D1: Safari, Firefox and phones stay
  //                   on vrend until each is tested)
  //   ?exec=vrend | webgpu | proto | auto: the visitor's own choice; ?exec=webgpu needs navigator.gpu and nothing else
  //                   (it is how the site is tried before the switch is thrown; the Worker still falls back to vrend
  //                   when it finds no device), ?exec=auto is the site rules again with the site's value ignored
  //   no WebGL2 for display.js -> "none" (no executor; the machine draws in software)
  // and a webgpu that fails to start falls back to vrend (the Worker's "fallback", a Worker error before "ready"),
  // a vrend that fails leaves the guest to software (gpuserver's rvgpu-0.none after 10 s).
  const EXEC_NAMES = ["vrend", "webgpu", "auto"];
  // What the page needs of the adapter, before it starts rvgpu (the Worker asks the adapter for all of its limits and
  // features anyway). Numbers are WebGPU's own minimums (what wgvk and the P4 profile ask for, gpu/webgpu/profiles/):
  // 4 bind groups, 8 colour attachments, 8 vertex buffers of 16 attributes, 8 storage buffers per stage, a 128 MiB
  // storage binding (Dawn's: EEVEE's shadow pool must fit, STATUS.md), 256 MiB buffers, 8192 px textures.
  // A software adapter (isFallbackAdapter) never passes: lavapipe-in-wasm is behind it already.
  const MIN_LIMITS = { maxBindGroups: 4, maxColorAttachments: 8, maxVertexBuffers: 8, maxVertexAttributes: 16,
                       maxStorageBuffersPerShaderStage: 8, maxStorageBufferBindingSize: 128 << 20,
                       maxBufferSize: 256 << 20, maxTextureDimension2D: 8192, maxStorageTexturesPerShaderStage: 4 };
  const MIN_FEATURES = [];
  const normExec = (v) => (typeof v === "string" && EXEC_NAMES.includes(v.toLowerCase()) ? v.toLowerCase() : null);
  // Pure: the adapter's side of the probe. adapter: {limits, features, isFallbackAdapter?, info?} (a GPUAdapter).
  const checkAdapter = (adapter) => {
    if (!adapter) {
      return { ok: false, reason: "no WebGPU adapter" };
    }
    if (adapter.isFallbackAdapter === true || (adapter.info && adapter.info.isFallbackAdapter === true)) {
      return { ok: false, reason: "a software (fallback) adapter" };
    }
    const bad = [];
    for (const k in MIN_LIMITS) {
      const v = adapter.limits ? adapter.limits[k] : undefined;
      if (!(v >= MIN_LIMITS[k])) {
        bad.push(k + " " + v + " < " + MIN_LIMITS[k]);
      }
    }
    for (const f of MIN_FEATURES) {
      if (!adapter.features || !adapter.features.has(f)) {
        bad.push("feature " + f);
      }
    }
    return bad.length ? { ok: false, reason: "adapter below the minimum: " + bad.join(", ") } : { ok: true, reason: "" };
  };
  // The adapter probe (nothing is created from it; the Worker makes the device). Never throws, gives up after `ms`.
  const probeAdapter = async (gpu, ms = 3000) => {
    if (!gpu) {
      return { ok: false, reason: "no navigator.gpu" };
    }
    try {
      const adapter = await Promise.race([gpu.requestAdapter({ powerPreference: "high-performance" }),
                                          new Promise((_, reject) => setTimeout(() => reject(new Error("timeout")), ms))]);
      return checkAdapter(adapter);
    } catch (error) {
      return { ok: false, reason: "requestAdapter: " + (error && error.message || error) };
    }
  };
  // D1: Chrome or Edge, on a desktop. userAgentData is Chromium's; Safari and Firefox have none, so they fail here.
  const browserAllowed = (nav) => {
    const uad = nav && nav.userAgentData;
    if (!uad || uad.mobile !== false || !Array.isArray(uad.brands)) {
      return false;
    }
    return uad.brands.some((b) => b && (b.brand === "Google Chrome" || b.brand === "Microsoft Edge"));
  };
  // Pure: the selection table. in: { override, site, hasGpu, hasGL2, browserOk, probe: {ok, reason} | null }.
  // Returns { exec: "vrend" | "webgpu" | "proto" | "none", why }.
  const selectExec = (i) => {
    if (i.hasGL2 === false) {
      return { exec: "none", why: "no WebGL2" };
    }
    const o = typeof i.override === "string" ? i.override.toLowerCase() : "";
    if (o === "proto") {
      return { exec: "proto", why: "?exec=proto" };
    }
    if (o === "vrend") {
      return { exec: "vrend", why: "?exec=vrend" };
    }
    if (o === "webgpu") {
      return i.hasGpu ? { exec: "webgpu", why: "?exec=webgpu" } : { exec: "vrend", why: "?exec=webgpu, but no navigator.gpu" };
    }
    const site = normExec(i.site) || "vrend";
    if (site === "vrend") {
      return { exec: "vrend", why: "site default vrend" };
    }
    if (!i.hasGpu) {
      return { exec: "vrend", why: "site " + site + ", but no navigator.gpu" };
    }
    if (site === "auto" && !i.browserOk) {
      return { exec: "vrend", why: "site auto, but this browser is not Chrome/Edge on a desktop (D1)" };
    }
    if (!i.probe || !i.probe.ok) {
      return { exec: "vrend", why: "site " + site + ", but the adapter check failed: " + (i.probe ? i.probe.reason : "not run") };
    }
    return { exec: "webgpu", why: "site " + site + ", browser and adapter pass" };
  };
  globalThis.rvGpuSelect = { selectExec, checkAdapter, probeAdapter, browserAllowed, normExec, MIN_LIMITS, MIN_FEATURES };

  // ---- TGSI text -> GLSL ES 3.00, for the few instructions the stub's shaders have (MOV, ADD, MUL, MAD,
  // DP4); everything else is refused, loudly. vrend's translator (vrend_shader.c) is what replaces this.
  const tgsiToGlsl = (text) => {
    const lines = text.split("\n").map((l) => l.trim()).filter((l) => l);
    const stage = lines[0];
    if (stage !== "VERT" && stage !== "FRAG") {
      throw new Error("gpu: shader: not VERT or FRAG: " + stage);
    }
    const decl = [], body = [];
    let consts = 0, temps = 0;
    const ins = new Map(), outs = new Map();
    const reg = (r) => {
      // IN[0], OUT[1], TEMP[0], CONST[0][3] (buffer 0, element 3), with an optional swizzle or write mask
      const m = /^(-)?(IN|OUT|TEMP|CONST|IMM)\[(\d+)\](?:\[(\d+)\])?(?:\.([xyzw]+))?$/.exec(r);
      if (!m) {
        throw new Error("gpu: shader: operand " + r);
      }
      const [, neg, file, a, b, sw] = m;
      let name;
      if (file === "CONST") {
        name = "c[" + (b !== undefined ? b : a) + "]";
      } else if (file === "TEMP") {
        name = "t" + a;
      } else if (file === "IN") {
        name = ins.get(+a);
      } else if (file === "OUT") {
        name = outs.get(+a);
      } else {
        throw new Error("gpu: shader: IMM not done");
      }
      return { text: (neg ? "-" : "") + name, swizzle: sw || "", name };
    };
    const src = (r) => {
      const o = reg(r);
      return o.swizzle ? o.text + "." + o.swizzle : o.text;
    };
    for (const line of lines.slice(1)) {
      let m;
      if ((m = /^DCL IN\[(\d+)\](?:, (\w+)(?:\[\d+\])?(?:, (\w+))?)?$/.exec(line))) {
        if (stage === "VERT") {
          decl.push("layout(location = " + m[1] + ") in vec4 a" + m[1] + ";");
          ins.set(+m[1], "a" + m[1]);
        } else {
          decl.push("in vec4 v" + m[1] + ";");
          ins.set(+m[1], "v" + m[1]);
        }
      } else if ((m = /^DCL OUT\[(\d+)\], (\w+)/.exec(line))) {
        if (stage === "VERT" && m[2] === "POSITION") {
          outs.set(+m[1], "gl_Position");
        } else if (stage === "VERT") {
          // (varyings are matched by their order: the fragment shader's IN[n] is the vertex shader's n-th
          // non-position output, which holds for the stub's shaders; vrend matches by semantic.)
          const n = outs.size - (outs.has(0) && outs.get(0) === "gl_Position" ? 1 : 0);
          decl.push("out vec4 v" + n + ";");
          outs.set(+m[1], "v" + n);
        } else {
          decl.push("layout(location = " + m[1] + ") out vec4 o" + m[1] + ";");
          outs.set(+m[1], "o" + m[1]);
        }
      } else if ((m = /^DCL CONST\[(?:\d+\]\[)?(\d+)(?:\.\.(\d+))?\]$/.exec(line))) {
        consts = Math.max(consts, (m[2] !== undefined ? +m[2] : +m[1]) + 1);
      } else if ((m = /^DCL TEMP\[(\d+)(?:\.\.(\d+))?\]$/.exec(line))) {
        temps = Math.max(temps, (m[2] !== undefined ? +m[2] : +m[1]) + 1);
      } else if (/^PROPERTY /.test(line)) {
        // (nothing the stub's shaders need)
      } else if ((m = /^(?:\d+: )?(MOV|ADD|MUL|MAD|DP4) (.+)$/.exec(line))) {
        const ops = m[2].split(/,\s*/);
        const dst = reg(ops[0]);
        const mask = dst.swizzle;
        const lhs = mask ? dst.name + "." + mask : dst.name;
        const a = ops.slice(1).map(src);
        let e;
        switch (m[1]) {
          case "MOV": e = a[0]; break;
          case "ADD": e = a[0] + " + " + a[1]; break;
          case "MUL": e = a[0] + " * " + a[1]; break;
          case "MAD": e = a[0] + " * " + a[1] + " + " + a[2]; break;
          case "DP4": e = "vec4(dot(" + a[0] + ", " + a[1] + "))"; break;
        }
        body.push(lhs + " = (" + e + ")" + (mask ? "." + mask : "") + ";");
      } else if (/^(?:\d+: )?END$/.test(line)) {
        break;
      } else {
        throw new Error("gpu: shader: not done: " + line);
      }
    }
    const head = ["#version 300 es", "precision highp float;"];
    if (consts) {
      head.push("uniform vec4 c[" + consts + "];");
    }
    for (let i = 0; i < temps; i++) {
      body.unshift("vec4 t" + i + " = vec4(0.0);");
    }
    return { stage, consts, glsl: head.concat(decl, ["void main() {"], body.map((l) => "  " + l), ["}"]).join("\n") };
  };



  // `done` (every record up to it finished) and, ring version 2, each context's `ctx_done` (its records up to
  // it finished): from what was handed to WebGL and what is still pending (in order). One notify after any
  // change, on `done` (PROTOCOL.md: gpuserver waits there).
  const publishDone = (ex, w32) => {
    const r = ex.ring;
    let changed = false;
    const done = ex.pending.length ? ex.pending[0].serial - 1 : ex.processed;
    if ((Atomics.load(w32, ix32(r + H_DONE)) >>> 0) !== (done >>> 0)) {
      Atomics.store(w32, ix32(r + H_DONE), done | 0);
      changed = true;
    }
    if (w32[ix32(r + H_VERSION)] >= 2) {
      const first = new Map();	// ctx -> its first pending serial
      for (const p of ex.pending) {
        if (!first.has(p.ctx)) {
          first.set(p.ctx, p.serial);
        }
      }
      for (const [ctx, last] of ex.ctxProcessed) {
        if (ctx < 1 || ctx > 255) {
          continue;
        }
        const d = first.has(ctx) ? first.get(ctx) - 1 : last;
        const at = ix32(r + H_CTX_DONE + 4 * ctx);
        if ((Atomics.load(w32, at) >>> 0) !== (d >>> 0)) {
          Atomics.store(w32, at, d | 0);
          changed = true;
        }
      }
    }
    if (changed) {
      Atomics.notify(w32, ix32(r + H_DONE));
    }
  };

  // ---- the executor: one per WebGL2 context; every guest context shares it (WebGL contexts share nothing).
  class Executor {
    constructor(gl, memory, ring) {
      this.gl = gl;
      this.memory = memory;
      this.ring = ring;
      this.resources = new Map();	// res (global, gpuserver's numbers) -> { target, w, h, addr, size, texture | buffer, fbo }
      this.objects = new Map();	// "ctx:handle" -> shader / surface / vertex elements
      this.programs = new Map();	// "ctx:vs:fs" -> { program, vc }
      this.state = new Map();	// ctx -> what is bound
      this.ctx = 0;
      this.vao = gl.createVertexArray();
      this.processed = 0;	// serial of the newest record handed to WebGL
      this.ctxProcessed = new Map();	// ctx -> serial of its newest record handed to WebGL
      this.pending = [];	// records not finished yet, in order: { serial, kind: "get" | "present", ... }
      this.stats = { records: 0, submits: 0, bytes: 0, draws: 0, clears: 0, puts: 0, putBytes: 0, gets: 0, getBytes: 0,
                     getMs: 0, presents: 0, execMs: 0, glCalls: 0, lastExecMs: 0, maxExecMs: 0, errors: [] };
      this.onPresent = null;
      this.sharedReads = true;	// getBufferSubData takes a view over the shared memory (if not: through a copy)
      const w32 = new Int32Array(memory.buffer);
      Atomics.store(w32, ix32(ring + H_CAPS_LEN), 0);	// (no caps yet: vrend's go here)
      Atomics.store(w32, ix32(ring + H_EXEC_STATE), 1);
    }
    key(handle) {
      return this.ctx + ":" + handle;
    }
    st() {
      let s = this.state.get(this.ctx);
      if (!s) {
        s = { vs: 0, fs: 0, ve: 0, vbufs: [], cbufs: [], viewport: [0, 0, 1, 1], consts: [null, null] };
        this.state.set(this.ctx, s);
      }
      return s;
    }
    error(e) {
      this.stats.errors.push(String(e && e.message || e));
      if (this.stats.errors.length > 20) {
        this.stats.errors.shift();
      }
    }

    // One turn: what is in the ring, then what has finished since the last turn.
    turn() {
      const w32 = new Int32Array(this.memory.buffer);
      const r = this.ring;
      Atomics.add(w32, ix32(r + H_EXEC_HEARTBEAT), 1);
      if (w32[ix32(r + H_MAGIC)] !== RING_MAGIC) {
        return;
      }
      const size = w32[ix32(r + H_DATA_SIZE)], data = r + w32[ix32(r + H_DATA_OFF)];
      const t0 = performance.now();
      let n = 0;
      for (;;) {
        const head = Atomics.load(w32, ix32(r + H_HEAD)) >>> 0, tail = Atomics.load(w32, ix32(r + H_TAIL)) >>> 0;
        if (head === tail) {
          break;
        }
        const at = tail % size;
        let used;
        if (size - at < 16) {
          used = size - at;	// (too little room even for a PAD's header: the producer starts again at 0)
        } else {
          const p = ix32(data + at);
          const op = w32[p], ctx = w32[p + 1], serial = w32[p + 2] >>> 0, bytes = w32[p + 3] >>> 0;
          // (version 2: payloads padded to 16 bytes; version 1: to 8)
          used = w32[ix32(r + H_VERSION)] >= 2 ? 16 + ((bytes + 15) & ~15) : 16 + ((bytes + 7) & ~7);
          if (op !== OP_PAD) {
            try {
              this.record(op, ctx, serial, p + 4, bytes, w32);
            } catch (error) {
              // (which record, and for a trap in vrend where in it: the wasm functions on the stack)
              const where = error && error.stack ? " at " + String(error.stack).split("\n").slice(1, 9)
                .map((l) => l.trim().replace(/^at /, "").replace(/ \(.*\)$/, "")).join(" < ") : "";
              this.error("record " + op + " (serial " + serial + ", ctx " + ctx + "): " + (error && error.message || error) + where);
            }
            this.processed = serial;
            this.ctxProcessed.set(ctx, serial);
            n++;
          }
        }
        Atomics.store(w32, ix32(r + H_TAIL), (tail + used) | 0);
      }
      if (n) {
        const dt = performance.now() - t0;
        this.stats.records += n;
        this.stats.execMs += dt;
        this.stats.lastExecMs = dt;
        this.stats.maxExecMs = Math.max(this.stats.maxExecMs, dt);
      }
      this.complete(w32);
    }

    // Raise `done` as far as the records in order allow.
    complete(w32) {
      const gl = this.gl;
      while (this.pending.length) {
        const p = this.pending[0];
        if (p.kind === "get") {
          const s = gl.clientWaitSync(p.fence, 0, 0);
          if (s !== gl.ALREADY_SIGNALED && s !== gl.CONDITION_SATISFIED) {
            break;
          }
          gl.deleteSync(p.fence);
          gl.bindBuffer(gl.PIXEL_PACK_BUFFER, p.pbo);
          this.readInto(p);
          gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
          gl.deleteBuffer(p.pbo);
          this.stats.getMs += performance.now() - p.t0;
        } else if (!p.shown) {
          break;
        }
        this.pending.shift();
      }
      publishDone(this, w32);
    }

    // A finished readback, from the pack buffer into the backing: straight into the shared memory if WebGL
    // takes such a view, else through a copy. Rows are the resource's width apart (PROTOCOL.md: the strides
    // virglrenderer uses when the client gives none).
    readInto(p) {
      const gl = this.gl;
      if (this.sharedReads) {
        try {
          gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, new Uint8Array(this.memory.buffer, p.dst, p.len));
          return;
        } catch (error) {
          this.sharedReads = false;
          this.error("getBufferSubData refuses shared views (" + error.message + "): copying");
        }
      }
      const tmp = new Uint8Array(p.len);
      gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, tmp);
      new Uint8Array(this.memory.buffer, p.dst, p.len).set(tmp);
    }

    record(op, ctx, serial, p, bytes, w32) {
      const gl = this.gl;
      this.ctx = ctx;
      switch (op) {
        case OP_CTX_CREATE:
        case OP_CTX_DESTROY:
        case OP_RES_ATTACH:
        case OP_RES_DETACH:
          break;	// (this decoder's resources are global)
        case OP_RES_CREATE: {
          // res, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags, size, addr
          const res = { target: w32[p + 1], format: w32[p + 2], bind: w32[p + 3], w: w32[p + 4], h: w32[p + 5],
                        flags: w32[p + 10], size: w32[p + 11] >>> 0, addr: (w32[p + 12] >>> 0) + (w32[p + 13] >>> 0) * 0x100000000 };
          if (res.target === TARGET_BUFFER) {
            res.buffer = gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, res.buffer);
            gl.bufferData(gl.ARRAY_BUFFER, res.w, gl.DYNAMIC_DRAW);
          } else {
            res.texture = gl.createTexture();
            gl.bindTexture(gl.TEXTURE_2D, res.texture);
            gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, res.w, res.h);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
            res.fbo = gl.createFramebuffer();
            gl.bindFramebuffer(gl.FRAMEBUFFER, res.fbo);
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, res.texture, 0);
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            this.stats.glCalls += 8;
          }
          this.resources.set(w32[p], res);
          break;
        }
        case OP_RES_UNREF: {
          const res = this.resources.get(w32[p]);
          if (res) {
            res.buffer && gl.deleteBuffer(res.buffer);
            res.texture && gl.deleteTexture(res.texture);
            res.fbo && gl.deleteFramebuffer(res.fbo);
            this.resources.delete(w32[p]);
          }
          break;
        }
        case OP_SUBMIT:
          this.stats.submits++;
          this.stats.bytes += bytes;
          this.submit(w32, p, bytes >> 2);
          break;
        case OP_TRANSFER_PUT:
          this.transferPut(w32, p);
          break;
        case OP_TRANSFER_GET:
          this.transferGet(w32, p, serial);
          break;
        case OP_PRESENT: {
          // res, surface, serial_of_frame, x, y, w, h: done when the page has drawn it
          const res = w32[p], frame = w32[p + 2];
          this.stats.presents++;
          const item = { serial, ctx, kind: "present", shown: false };
          this.pending.push(item);
          if (this.onPresent) {
            this.onPresent(ctx, res, frame, serial, this.resources.get(res));
          }
          break;
        }
        default:
          throw new Error("gpu: record " + op + " not done here");
      }
    }

    // The page has drawn the PRESENT record `serial` (and so every one before it).
    shown(serial) {
      for (const p of this.pending) {
        if (p.kind === "present" && p.serial <= serial) {
          p.shown = true;
        }
      }
      this.complete(new Int32Array(this.memory.buffer));
    }

    // Data from the backing into the GPU resource: res, level, x, y, z, w, h, d, offset, data_size.
    transferPut(w32, p) {
      const gl = this.gl;
      const res = this.resources.get(w32[p]);
      const x = w32[p + 2], y = w32[p + 3], w = w32[p + 5], h = w32[p + 6], offset = w32[p + 8] >>> 0;
      if (res.target === TARGET_BUFFER) {
        gl.bindBuffer(gl.ARRAY_BUFFER, res.buffer);
        gl.bufferSubData(gl.ARRAY_BUFFER, x, new Uint8Array(this.memory.buffer, res.addr + offset, w));
        this.stats.putBytes += w;
      } else {
        // Rows res.w pixels apart in the backing, `offset` at the box's first pixel.
        gl.bindTexture(gl.TEXTURE_2D, res.texture);
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, res.w);
        gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
        gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, w, h, gl.RGBA, gl.UNSIGNED_BYTE,
                         new Uint8Array(this.memory.buffer, res.addr + offset, ((h - 1) * res.w + w) * 4));
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
        this.stats.putBytes += w * h * 4;
      }
      this.stats.puts++;
      this.stats.glCalls += 4;
    }

    // From the GPU resource into the backing, later (completion): readPixels into a pack buffer and a fence now.
    transferGet(w32, p, serial) {
      const gl = this.gl;
      const res = this.resources.get(w32[p]);
      const x = w32[p + 2], y = w32[p + 3], w = w32[p + 5], h = w32[p + 6], offset = w32[p + 8] >>> 0;
      const len = ((h - 1) * res.w + w) * 4;
      const pbo = gl.createBuffer();
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
      gl.bufferData(gl.PIXEL_PACK_BUFFER, len, gl.STREAM_READ);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, res.fbo);
      gl.pixelStorei(gl.PACK_ROW_LENGTH, res.w);
      gl.readPixels(x, y, w, h, gl.RGBA, gl.UNSIGNED_BYTE, 0);
      gl.pixelStorei(gl.PACK_ROW_LENGTH, 0);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
      const fence = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
      gl.flush();
      this.pending.push({ serial, ctx: this.ctx, kind: "get", fence, pbo, dst: res.addr + offset, len, t0: performance.now() });
      this.stats.gets++;
      this.stats.getBytes += w * h * 4;
      this.stats.glCalls += 10;
    }

    // virgl's command stream (`n` words at word index `p`).
    submit(w32, p, n0) {
      const end = p + n0;
      const f32 = new Float32Array(w32.buffer);
      while (p < end) {
        const h = w32[p] >>> 0;
        const cmd = h & 0xff, obj = (h >> 8) & 0xff, n = h >>> 16;
        const a = p + 1;
        switch (cmd) {
          case 0:
            break;
          case CCMD_CREATE_OBJECT: this.createObject(obj, w32, a, n); break;
          case CCMD_BIND_OBJECT:
            if (obj === OBJ_VERTEX_ELEMENTS) {
              this.st().ve = w32[a];
            }
            break;
          case CCMD_DESTROY_OBJECT: this.objects.delete(this.key(w32[a])); break;
          case CCMD_SET_VIEWPORT_STATE: {
            // start slot, then scale and translate (x, y, z each): a viewport of x = tx - sx, w = 2 sx
            const sx = f32[a + 1], sy = f32[a + 2], tx = f32[a + 4], ty = f32[a + 5];
            this.st().viewport = [tx - sx, ty - Math.abs(sy), 2 * sx, 2 * Math.abs(sy)];
            break;
          }
          case CCMD_SET_FRAMEBUFFER_STATE: {
            const nr = w32[a];
            this.st().cbufs = Array.from({ length: nr }, (_, i) => w32[a + 2 + i]);
            break;
          }
          case CCMD_SET_VERTEX_BUFFERS: {
            const vb = [];
            for (let i = 0; i < n / 3; i++) {
              vb.push({ stride: w32[a + 3 * i], offset: w32[a + 3 * i + 1], handle: w32[a + 3 * i + 2] });
            }
            this.st().vbufs = vb;
            break;
          }
          case CCMD_CLEAR: this.clear(w32, f32, a); break;
          case CCMD_DRAW_VBO: this.draw(w32, a); break;
          case CCMD_RESOURCE_INLINE_WRITE: this.inlineWrite(w32, a, n); break;
          case CCMD_SET_CONSTANT_BUFFER:
            this.st().consts[w32[a]] = f32.slice(a + 2, a + n);
            break;
          case CCMD_BIND_SHADER: {
            const handle = w32[a], type = w32[a + 1];
            if (type === SHADER_VERTEX) {
              this.st().vs = handle;
            } else if (type === SHADER_FRAGMENT) {
              this.st().fs = handle;
            }
            break;
          }
          case CCMD_SET_SUB_CTX:
          case CCMD_CREATE_SUB_CTX:
            break;
          default:
            throw new Error("gpu: virgl command " + cmd + " not done here");
        }
        p = a + n;
      }
    }

    createObject(obj, w32, a, n) {
      const handle = w32[a];
      if (obj === OBJ_SHADER) {
        // handle, type, offset, num_tokens, so_num_outputs, then the text, NUL-terminated, padded to words
        const type = w32[a + 1];
        const bytes = new Uint8Array(w32.buffer, (a + 5) * 4, (n - 5) * 4);
        let e = bytes.indexOf(0);
        if (e < 0) {
          e = bytes.length;
        }
        const t = tgsiToGlsl(new TextDecoder().decode(bytes.slice(0, e)));
        const gl = this.gl;
        const s = gl.createShader(type === SHADER_VERTEX ? gl.VERTEX_SHADER : gl.FRAGMENT_SHADER);
        gl.shaderSource(s, t.glsl);
        gl.compileShader(s);
        this.objects.set(this.key(handle), { kind: "shader", type, shader: s, glsl: t.glsl });
      } else if (obj === OBJ_SURFACE) {
        this.objects.set(this.key(handle), { kind: "surface", res: w32[a + 1], format: w32[a + 2] });
      } else if (obj === OBJ_VERTEX_ELEMENTS) {
        const ve = [];
        for (let i = 0; i < (n - 1) / 4; i++) {
          const b = a + 1 + 4 * i;
          ve.push({ offset: w32[b], divisor: w32[b + 1], vb: w32[b + 2], format: w32[b + 3] });
        }
        this.objects.set(this.key(handle), { kind: "ve", ve });
      } else if (obj >= 1 && obj <= 3) {
        // blend, rasterizer, depth-stencil: this decoder draws with fixed state (no blending, no depth)
      } else {
        throw new Error("gpu: object type " + obj + " not done here");
      }
    }

    program(vs, fs) {
      const k = this.ctx + ":" + vs + ":" + fs;
      let p = this.programs.get(k);
      if (p) {
        return p;
      }
      const gl = this.gl;
      const v = this.objects.get(this.key(vs)), f = this.objects.get(this.key(fs));
      const program = gl.createProgram();
      gl.attachShader(program, v.shader);
      gl.attachShader(program, f.shader);
      gl.linkProgram(program);
      // (asked once per program: every query is a round trip to the GPU process)
      if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error("gpu: program: " + gl.getProgramInfoLog(program) + " | vs: " + gl.getShaderInfoLog(v.shader)
          + " | fs: " + gl.getShaderInfoLog(f.shader));
      }
      p = { program, vc: gl.getUniformLocation(program, "c") };
      this.programs.set(k, p);
      return p;
    }

    bindTarget() {
      const gl = this.gl, s = this.st();
      const surf = this.objects.get(this.key(s.cbufs[0]));
      const res = surf && this.resources.get(surf.res);
      gl.bindFramebuffer(gl.FRAMEBUFFER, res ? res.fbo : null);
      gl.viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
      this.stats.glCalls += 2;
      return res;
    }

    clear(w32, f32, a) {
      const gl = this.gl;
      this.bindTarget();
      gl.disable(gl.SCISSOR_TEST);
      gl.colorMask(true, true, true, true);
      if (w32[a] & CLEAR_COLOR0) {
        gl.clearColor(f32[a + 1], f32[a + 2], f32[a + 3], f32[a + 4]);
        gl.clear(gl.COLOR_BUFFER_BIT);
      }
      this.stats.clears++;
      this.stats.glCalls += 4;
    }

    draw(w32, a) {
      const gl = this.gl, s = this.st();
      const start = w32[a], count = w32[a + 1], mode = w32[a + 2];
      if (w32[a + 3]) {
        throw new Error("gpu: indexed draws not done here");
      }
      this.bindTarget();
      const p = this.program(s.vs, s.fs);
      gl.useProgram(p.program);
      gl.bindVertexArray(this.vao);
      gl.disable(gl.BLEND);
      gl.disable(gl.DEPTH_TEST);
      gl.disable(gl.CULL_FACE);
      const consts = s.consts[SHADER_VERTEX];
      if (consts && p.vc) {
        gl.uniform4fv(p.vc, consts);
        this.stats.glCalls++;
      }
      const ve = this.objects.get(this.key(s.ve));
      ve.ve.forEach((e, i) => {
        const vb = s.vbufs[e.vb];
        const res = this.resources.get(vb.handle);
        const size = { [FORMAT_R32G32_FLOAT]: 2, [FORMAT_R32G32B32_FLOAT]: 3, [FORMAT_R32G32B32A32_FLOAT]: 4 }[e.format];
        gl.bindBuffer(gl.ARRAY_BUFFER, res.buffer);
        gl.enableVertexAttribArray(i);
        gl.vertexAttribPointer(i, size, gl.FLOAT, false, vb.stride, vb.offset + e.offset);
        gl.vertexAttribDivisor(i, e.divisor);
        this.stats.glCalls += 4;
      });
      gl.drawArrays(PRIMS[mode], start, count);
      this.stats.glCalls += 8;
      this.stats.draws++;
    }

    // Data inline in the stream (res, level, usage, stride, layer_stride, x, y, z, w, h, d, data).
    inlineWrite(w32, a, n) {
      const gl = this.gl;
      const res = this.resources.get(w32[a]);
      const x = w32[a + 5], y = w32[a + 6], w = w32[a + 8], h = w32[a + 9];
      const data = new Uint8Array(w32.buffer, (a + 11) * 4, (n - 11) * 4);
      if (res.target === TARGET_BUFFER) {
        gl.bindBuffer(gl.ARRAY_BUFFER, res.buffer);
        gl.bufferSubData(gl.ARRAY_BUFFER, x, data, 0, w);
      } else {
        gl.bindTexture(gl.TEXTURE_2D, res.texture);
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, w32[a + 3] / 4);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, w, h, gl.RGBA, gl.UNSIGNED_BYTE, data);
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
      }
      this.stats.glCalls += 2;
    }

    texture(res) {
      const r = this.resources.get(res);
      // (GL's way up, bottom row first, unless the resource was made top row first: VIRGL_RESOURCE_Y_0_TOP,
      // vrend's y_0_top, which vrend draws into upside down so that its rows are in memory order)
      return r && r.texture ? { texture: r.texture, flipY: !(r.flags & 1), w: r.w, h: r.h } : null;
    }
  }


  // ---- the real executor: virglrenderer's vrend built to WebAssembly (gpu/vrend, vrend.js + vrend.wasm), on
  // a WebGL2 context of its own. The records' data is copied into the module's heap (it has a memory of its
  // own) and read data copied back out; vrend's readbacks are synchronous (glReadPixels), so every record but
  // PRESENT is done when it returns. A present blits the resource's texture into the canvas (in a Worker,
  // which then posts it to the page as an ImageBitmap); the frame buffer bindings and the scissor test are put
  // back afterwards, since vrend keeps its own idea of them.
  class VrendExecutor {
    constructor(gl, memory, ring, M, caps) {
      this.gl = gl;
      this.memory = memory;
      this.ring = ring;
      this.M = M;
      this.resources = new Map();	// res -> { ctx, target, w, h, flags, addr, size }
      this.processed = 0;
      this.ctxProcessed = new Map();
      this.pending = [];
      this.blitFbo = gl.createFramebuffer();
      this.stats = { records: 0, submits: 0, bytes: 0, puts: 0, putBytes: 0, gets: 0, getBytes: 0, getMs: 0,
                     presents: 0, execMs: 0, lastExecMs: 0, maxExecMs: 0, copyMs: 0, errors: [] };
      this.onPresent = null;
      const handle = M.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: 1 });
      M.GL.makeContextCurrent(handle);
      const r = M._rvg_init();
      const w32 = new Int32Array(memory.buffer);
      if (r) {
        this.error("virgl_renderer_init failed: " + r);
        Atomics.store(w32, ix32(ring + H_EXEC_STATE), 3);
        return;
      }
      // The caps where gpuserver said (PROTOCOL.md: caps, capsSize), then ready.
      const n = M._rvg_caps();
      if (caps && caps.at && n <= caps.size) {
        new Uint8Array(memory.buffer, caps.at, n).set(M.HEAPU8.subarray(M._rvg_buffer(0), M._rvg_buffer(0) + n));
        Atomics.store(w32, ix32(ring + H_CAPS_LEN), n);
      }
      this.capsLen = n;
      Atomics.store(w32, ix32(ring + H_EXEC_STATE), 1);
    }
    error(e) {
      const t = String(e && e.message || e);
      if ((this.stats.errorsFirst = this.stats.errorsFirst || []).length < 10) {
        this.stats.errorsFirst.push(t);	// (the first ones say what went wrong first)
      }
      this.stats.errors.push(t);
      if (this.stats.errors.length > 20) {
        this.stats.errors.shift();
      }
    }
    // `bytes` of the machine's memory at `at` into the module's buffer; returns where it is there.
    toHeap(at, bytes) {
      const b = this.M._rvg_buffer(bytes);
      this.M.HEAPU8.set(new Uint8Array(this.memory.buffer, at, bytes), b);
      return b;
    }
    turn() {
      // Queries: vrend asks WebGL for a query's result when the guest asks for it (GET_QUERY_RESULT), and WebGL
      // never has it within the task that ended the query; vrend then keeps the query on a list that it looks at
      // again only when a fence of its own retires (vrend_renderer_check_fences). Without that, Mesa's
      // virgl_get_query_result read the query's buffer forever (mpv's timer queries: the player stood still
      // after its first frame). So: a fence on every turn that has none outstanding, and a poll, which retires
      // the last one (WebGL's fence is signalled by now: a turn is a later task) and so looks at the queries.
      if (this.M._rvg_fence && this.M._rvg_poll) {
        const done = this.M._rvg_poll() >>> 0;
        if (!this.queryFence || done >= this.queryFence) {
          this.queryFence = (this.queryFence || 0) + 1;
          this.M._rvg_fence(this.queryFence, 0);
        }
      }
      Executor.prototype.turn.call(this);
    }
    complete(w32) {
      while (this.pending.length && this.pending[0].shown) {
        this.pending.shift();
      }
      publishDone(this, w32);
    }
    shown(serial) {
      Executor.prototype.shown.call(this, serial);
    }
    record(op, ctx, serial, p, bytes, w32) {
      const M = this.M;
      let r = 0;
      const t0 = performance.now();
      switch (op) {
        case OP_CTX_CREATE: {
          const b = new Uint8Array(this.memory.buffer, p * 4, bytes);
          let n = b.indexOf(0);
          n = n < 0 ? bytes : n;
          this.toHeap(p * 4, n);
          r = M._rvg_ctx_create(ctx, n);
          break;
        }
        case OP_CTX_DESTROY:
          M._rvg_ctx_destroy(ctx);
          break;
        case OP_RES_CREATE: {
          const res = w32[p];
          const v = (i) => w32[p + i] >>> 0;
          r = M._rvg_res_create(ctx, res, v(1), v(2), v(3), v(4), v(5), v(6), v(7), v(8), v(9), v(10));
          this.resources.set(res, { ctx, target: v(1), format: v(2), w: v(4), h: v(5), flags: v(10), size: v(11),
                                    addr: v(12) + v(13) * 0x100000000 });
          break;
        }
        case OP_RES_UNREF:
          M._rvg_res_unref(ctx, w32[p]);
          this.resources.delete(w32[p]);
          break;
        case OP_RES_ATTACH:
          // (another connection's resource imported by this context: a dmabuf between processes, or libva's
          // video surfaces in a player's GL)
          r = M._rvg_res_attach ? M._rvg_res_attach(ctx, w32[p]) : -1;
          break;
        case OP_RES_DETACH:
          if (M._rvg_res_detach) {
            M._rvg_res_detach(ctx, w32[p]);
          }
          break;
        case OP_SUBMIT:
          this.stats.submits++;
          this.stats.bytes += bytes;
          this.toHeap(p * 4, bytes);
          this.stats.copyMs += performance.now() - t0;
          r = M._rvg_submit(ctx, bytes >> 2);
          break;
        case OP_TRANSFER_PUT:
        case OP_TRANSFER_GET: {
          // res, level, x, y, z, w, h, d, offset, data_size
          const v = (i) => w32[p + i] >>> 0;
          const res = this.resources.get(v(0));
          const len = v(9);
          if (!res || res.addr + v(8) + len > this.memory.buffer.byteLength) {
            throw new Error("gpu: transfer outside its backing (res " + v(0) + ")");
          }
          if (op === OP_TRANSFER_PUT) {
            this.toHeap(res.addr + v(8), len);
            r = M._rvg_put(ctx, v(0), v(1), v(2), v(3), v(4), v(5), v(6), v(7), len);
            this.stats.puts++;
            this.stats.putBytes += len;
          } else {
            r = M._rvg_get(ctx, v(0), v(1), v(2), v(3), v(4), v(5), v(6), v(7), len);
            const b = M._rvg_buffer(0);
            new Uint8Array(this.memory.buffer, res.addr + v(8), len).set(M.HEAPU8.subarray(b, b + len));
            this.stats.gets++;
            this.stats.getBytes += len;
            this.stats.getMs += performance.now() - t0;
          }
          break;
        }
        case OP_PRESENT: {
          const res = w32[p], frame = w32[p + 2];
          this.stats.presents++;
          this.pending.push({ serial, ctx, kind: "present", shown: false });
          if (this.onPresent) {
            this.onPresent(ctx, res, frame, serial, this.present(res));
          }
          break;
        }
        default:
          throw new Error("gpu: record " + op + " not done here");
      }
      if (r) {
        // (with the record's first words: which resource, what kind of one)
        const words = Array.from(w32.subarray(p, p + Math.min(bytes >> 2, op === OP_SUBMIT ? 4 : 14)), (x) => x >>> 0);
        this.error("record " + op + " (serial " + serial + ", ctx " + ctx + "): " + r + " [" + words.join(" ") + "]");
      }
    }
    // What a present needs: the resource's texture (a WebGLTexture of this context) and size.
    present(res) {
      const name = this.M._rvg_res_texture(res);
      const r = this.resources.get(res);
      if (!name || !r) {
        return null;
      }
      // (B8G8R8A8/X8, Wayland's ARGB8888/XRGB8888: vrend on GL ES keeps them as RGBA textures with the bytes in
      // memory order, B in red, and swizzles in its shaders; whoever samples the texture must swizzle too)
      // (row 0 of vrend's texture is the top of the picture: Mesa's state tracker draws a window's buffer upside
      // down for that, and vrend keeps Y_0_TOP resources in memory order too. Measured with present-test.py's
      // band on a dmabuf window: flipping here put it at the bottom.)
      return { texture: this.M.GL.textures[name], w: r.w, h: r.h, flipY: false,
               bgra: r.format === 1 || r.format === 2 };
    }
    // The resource's texture into the canvas's own buffer (the Worker's present).
    blitToCanvas(t) {
      const gl = this.gl;
      const read = gl.getParameter(gl.READ_FRAMEBUFFER_BINDING), draw = gl.getParameter(gl.DRAW_FRAMEBUFFER_BINDING);
      const scissor = gl.isEnabled(gl.SCISSOR_TEST);
      gl.disable(gl.SCISSOR_TEST);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.blitFbo);
      gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t.texture, 0);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
      // (GL's way up in the texture, unless Y_0_TOP: the canvas wants its top row at the top)
      if (t.flipY) {
        gl.blitFramebuffer(0, 0, t.w, t.h, 0, 0, t.w, t.h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
      } else {
        gl.blitFramebuffer(0, 0, t.w, t.h, 0, t.h, t.w, 0, gl.COLOR_BUFFER_BIT, gl.NEAREST);
      }
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, read);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, draw);
      if (scissor) {
        gl.enable(gl.SCISSOR_TEST);
      }
    }
    texture() {
      return null;	// (Worker only: the page has the imported bitmaps)
    }

    // ---- video frames (tools/www/vdec-worker.js): a decoded VideoFrame into a guest surface's two GPU resources,
    // luma (R8, w x h) and chroma (R8G8, w/2 x h/2), which the guest's GL samples through EGL dmabuf import. The
    // frame goes into a texture of ours (texImage2D: the browser's YUV to RGB, by the frame's colour space), then
    // two draws turn it back into Y and CbCr with the inverse of that matrix (chroma: the mean of each 2x2 block).
    // m.exact: copyTo NV12 and texSubImage2D instead (the bytes exactly, at a readback and an upload a frame).
    // Then the slot's "ready" word is the frame's chunk: the guest's vaSyncSurface returns, and every GL command it
    // sends after that runs after this (one executor, in order).
    vdecFrame(m) {
      const frame = m.frame;
      const done = () => {
        frame.close();
        Atomics.store(new Int32Array(this.memory.buffer), ix32(m.ready), m.seq | 0);
        this.stats.vdecFrames = (this.stats.vdecFrames || 0) + 1;
      };
      // (records already in the ring first: the guest's draws from this surface's last picture were sent before)
      this.turn();
      const ty = this.M._rvg_res_texture(m.res[0]), tuv = this.M._rvg_res_texture(m.res[1]);
      if (!ty || !tuv) {
        this.error("vdec: surface resources " + m.res + " have no textures");
        done();
        return;
      }
      const texY = this.M.GL.textures[ty], texUV = this.M.GL.textures[tuv];
      const w = Math.min(m.w || frame.displayWidth, frame.displayWidth), h = Math.min(m.h || frame.displayHeight, frame.displayHeight);
      if (m.exact || frame.displayWidth !== frame.visibleRect.width || frame.displayHeight !== frame.visibleRect.height) {
        this.vdecExact(frame, texY, texUV, w & ~1, h & ~1).then(done, (error) => {
          this.error("vdec: copyTo: " + (error && error.message || error));
          done();
        });
        return;
      }
      try {
        this.vdecDraw(frame, texY, texUV, w, h);
      } catch (error) {
        this.error("vdec: draw: " + (error && error.message || error));
      }
      done();
    }
    vdecSetup() {
      if (this.vd) {
        return this.vd;
      }
      const gl = this.gl;
      const vs = "#version 300 es\nvoid main() { vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 4.0 - 1.0;"
        + " gl_Position = vec4(p, 0.0, 1.0); }";
      const fs = (body) => "#version 300 es\nprecision highp float;\nuniform highp sampler2D rgb;\nuniform mat3 m;"
        + "\nuniform vec3 off;\nuniform ivec2 size;\nout vec4 o;\nvoid main() {\n" + body + "\n}";
      const lumaFs = fs("ivec2 p = ivec2(gl_FragCoord.xy); vec3 c = texelFetch(rgb, p, 0).rgb;"
        + " o = vec4((m * c + off).x, 0.0, 0.0, 1.0);");
      const chromaFs = fs("ivec2 p = ivec2(gl_FragCoord.xy) * 2; ivec2 q = min(p + 1, size - 1);"
        + " vec3 c = (texelFetch(rgb, p, 0).rgb + texelFetch(rgb, ivec2(q.x, p.y), 0).rgb"
        + " + texelFetch(rgb, ivec2(p.x, q.y), 0).rgb + texelFetch(rgb, q, 0).rgb) * 0.25;"
        + " vec3 yuv = m * c + off; o = vec4(yuv.y, yuv.z, 0.0, 1.0);");
      const program = (f) => {
        const p = gl.createProgram();
        for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, f]]) {
          const sh = gl.createShader(type);
          gl.shaderSource(sh, src);
          gl.compileShader(sh);
          gl.attachShader(p, sh);
        }
        gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
          throw new Error("vdec program: " + gl.getProgramInfoLog(p));
        }
        return { p, rgb: gl.getUniformLocation(p, "rgb"), m: gl.getUniformLocation(p, "m"),
                 off: gl.getUniformLocation(p, "off"), size: gl.getUniformLocation(p, "size") };
      };
      this.vd = { luma: program(lumaFs), chroma: program(chromaFs), vao: gl.createVertexArray(), fbo: gl.createFramebuffer(),
                  rgb: gl.createTexture() };
      return this.vd;
    }
    // The GL state vrend relies on, kept around our draws (vrend keeps its own idea of it).
    vdecSave() {
      const gl = this.gl, P = (x) => gl.getParameter(x);
      return { draw: P(gl.DRAW_FRAMEBUFFER_BINDING), read: P(gl.READ_FRAMEBUFFER_BINDING), viewport: P(gl.VIEWPORT),
        program: P(gl.CURRENT_PROGRAM), vao: P(gl.VERTEX_ARRAY_BINDING), active: P(gl.ACTIVE_TEXTURE),
        caps: [gl.SCISSOR_TEST, gl.BLEND, gl.DEPTH_TEST, gl.STENCIL_TEST, gl.CULL_FACE, gl.RASTERIZER_DISCARD,
               gl.DITHER, gl.POLYGON_OFFSET_FILL, gl.SAMPLE_ALPHA_TO_COVERAGE, gl.SAMPLE_COVERAGE].map((c) => [c, gl.isEnabled(c)]),
        mask: P(gl.COLOR_WRITEMASK), unpackBuffer: P(gl.PIXEL_UNPACK_BUFFER_BINDING),
        unpack: [gl.UNPACK_FLIP_Y_WEBGL, gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, gl.UNPACK_COLORSPACE_CONVERSION_WEBGL,
                 gl.UNPACK_ROW_LENGTH, gl.UNPACK_SKIP_ROWS, gl.UNPACK_SKIP_PIXELS, gl.UNPACK_ALIGNMENT,
                 gl.UNPACK_IMAGE_HEIGHT, gl.UNPACK_SKIP_IMAGES].map((k) => [k, P(k)]),
        tex0: (gl.activeTexture(gl.TEXTURE0), P(gl.TEXTURE_BINDING_2D)), sampler0: P(gl.SAMPLER_BINDING) };
    }
    vdecRestore(s) {
      const gl = this.gl;
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, s.draw);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, s.read);
      gl.viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
      gl.useProgram(s.program);
      gl.bindVertexArray(s.vao);
      for (const [c, on] of s.caps) {
        on ? gl.enable(c) : gl.disable(c);
      }
      gl.colorMask(s.mask[0], s.mask[1], s.mask[2], s.mask[3]);
      gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, s.unpackBuffer);
      for (const [k, v] of s.unpack) {
        gl.pixelStorei(k, v);
      }
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, s.tex0);
      gl.bindSampler(0, s.sampler0);
      gl.activeTexture(s.active);
    }
    vdecPlain() {
      const gl = this.gl;
      for (const c of [gl.SCISSOR_TEST, gl.BLEND, gl.DEPTH_TEST, gl.STENCIL_TEST, gl.CULL_FACE, gl.RASTERIZER_DISCARD,
                       gl.DITHER, gl.POLYGON_OFFSET_FILL, gl.SAMPLE_ALPHA_TO_COVERAGE, gl.SAMPLE_COVERAGE]) {
        gl.disable(c);
      }
      gl.colorMask(true, true, true, true);
      gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, null);
      gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
      gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
      gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
      gl.pixelStorei(gl.UNPACK_IMAGE_HEIGHT, 0);
      gl.pixelStorei(gl.UNPACK_SKIP_IMAGES, 0);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindSampler(0, null);
    }
    vdecDraw(frame, texY, texUV, w, h) {
      const gl = this.gl, vd = this.vdecSetup(), saved = this.vdecSave();
      try {
        this.vdecPlain();
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
        gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
        gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.NONE);
        gl.bindTexture(gl.TEXTURE_2D, vd.rgb);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, frame);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        const { m, off } = vdecMatrix(frame.colorSpace);
        gl.bindVertexArray(vd.vao);
        gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, vd.fbo);
        for (const [prog, tex, vw, vh] of [[vd.luma, texY, w, h], [vd.chroma, texUV, (w + 1) >> 1, (h + 1) >> 1]]) {
          gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
          gl.viewport(0, 0, vw, vh);
          gl.useProgram(prog.p);
          gl.uniform1i(prog.rgb, 0);
          gl.uniformMatrix3fv(prog.m, false, m);
          gl.uniform3fv(prog.off, off);
          gl.uniform2i(prog.size, frame.displayWidth, frame.displayHeight);
          gl.drawArrays(gl.TRIANGLES, 0, 3);
        }
        gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, null, 0);
      } finally {
        this.vdecRestore(saved);
      }
    }
    async vdecExact(frame, texY, texUV, w, h) {
      const rect = { x: 0, y: 0, width: w, height: h };
      const cw = w / 2, ch = h / 2;
      let y, uv;
      if (frame.format === "NV12") {
        const b = new Uint8Array(w * h + 2 * cw * ch);
        await frame.copyTo(b, { rect, layout: [{ offset: 0, stride: w }, { offset: w * h, stride: w }] });
        y = b.subarray(0, w * h);
        uv = b.subarray(w * h);
      } else {
        const b = new Uint8Array(w * h * 3 / 2);
        await frame.copyTo(b, { rect, layout: [{ offset: 0, stride: w }, { offset: w * h, stride: cw },
          { offset: w * h + cw * ch, stride: cw }] });
        y = b.subarray(0, w * h);
        uv = new Uint8Array(2 * cw * ch);
        for (let i = 0; i < cw * ch; i++) {
          uv[2 * i] = b[w * h + i];
          uv[2 * i + 1] = b[w * h + cw * ch + i];
        }
      }
      const gl = this.gl, saved = this.vdecSave();
      try {
        this.vdecPlain();
        gl.bindTexture(gl.TEXTURE_2D, texY);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RED, gl.UNSIGNED_BYTE, y);
        gl.bindTexture(gl.TEXTURE_2D, texUV);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, cw, ch, gl.RG, gl.UNSIGNED_BYTE, uv);
      } finally {
        this.vdecRestore(saved);
      }
    }
  }

  // The inverse of the browser's YUV to RGB for a frame's colour space: Y'CbCr from R'G'B' (Rec. ITU-T H.273,
  // the matrix's Kr and Kb), as a column-major mat3 and an offset, in 0..1 units (limited range: 16..235, 16..240).
  const vdecMatrix = (cs) => {
    const K = { bt709: [0.2126, 0.0722], bt470bg: [0.299, 0.114], smpte170m: [0.299, 0.114], "bt2020-ncl": [0.2627, 0.0593] };
    const full = !cs || cs.fullRange !== false;
    if (cs && cs.matrix === "rgb") {
      // (identity: G is Y, B is Cb, R is Cr)
      return { m: new Float32Array([0, 0, 1, 1, 0, 0, 0, 1, 0]), off: new Float32Array([0, 0, 0]) };
    }
    const [kr, kb] = K[cs && cs.matrix] || K.bt709, kg = 1 - kr - kb;
    const rows = [[kr, kg, kb], [-kr / (2 * (1 - kb)), -kg / (2 * (1 - kb)), 0.5], [0.5, -kg / (2 * (1 - kr)), -kb / (2 * (1 - kr))]];
    const sy = full ? 1 : 219 / 255, sc = full ? 1 : 224 / 255;
    const scale = [sy, sc, sc];
    const m = new Float32Array(9);
    for (let r = 0; r < 3; r++) {
      for (let c = 0; c < 3; c++) {
        m[c * 3 + r] = rows[r][c] * scale[r];
      }
    }
    return { m, off: new Float32Array([full ? 0 : 16 / 255, 128 / 255, 128 / 255]) };
  };

  // Turns on every doorbell (Atomics.waitAsync: neither thread blocks), and every 50 ms besides (1 ms while
  // a readback is in flight), for readbacks that finish between doorbells and for the heartbeat.
  const run = (ex) => {
    const w32 = new Int32Array(ex.memory.buffer);
    const bell = ix32(ex.ring + H_DOORBELL);
    const wait = () => {
      const seen = Atomics.load(w32, bell);
      ex.turn();
      // (1 ms while a readback is in flight: its fence is only polled on turns)
      const timeout = ex.pending.some((p) => p.kind === "get") ? 1 : 50;
      if (typeof Atomics.waitAsync !== "function") {
        // (Firefox before 145 has no Atomics.waitAsync: look again after the timeout, as the heartbeat does anyway;
        // a doorbell then waits up to `timeout` ms. Chrome and current Firefox never come here.)
        setTimeout(wait, timeout);
        return;
      }
      const r = Atomics.waitAsync(w32, bell, seen, timeout);
      (r.async ? r.value : Promise.resolve()).then(wait);
    };
    wait();
  };

  const renderer = (gl) => {
    const ext = gl.getExtension("WEBGL_debug_renderer_info");
    return ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
  };

  // ---- in a Worker: a context of our own; presents go to the page as ImageBitmaps.
  if (inWorker) {
    let ex = null, canvas = null, gl = null, startMemory = null;
    const watch = { table: null, timer: 0, sent: new Map(), hidden: false };	// slot -> the present sent for it
    const S_SEQ = 0, S_ID = 4, S_PRESENT = 56, S_BUFFER_LO = 60, S_BUFFER_HI = 64, S_FORMAT = 72, S_GPU_SERIAL = 244;
    const watchTable = () => {
      const t = watch.table;
      // (Not while the page is hidden: it draws nothing then, and a frame is sent for each window when it is
      // shown again, its present being newer than the one sent last.)
      if (!ex || !ex.grab || !t || watch.hidden) {
        return;
      }
      const w32 = new Int32Array(ex.memory.buffer);
      if (t.address + t.header + t.slots * t.slotSize > w32.byteLength) {
        return;
      }
      const done = Atomics.load(w32, ix32(ex.ring + H_DONE));
      for (let i = 0; i < t.slots; i++) {
        const at = t.address + t.header + i * t.slotSize;
        const seq = Atomics.load(w32, ix32(at));
        if ((seq & 1) || !w32[ix32(at + S_ID)] || w32[ix32(at + S_FORMAT)] !== 2) {
          continue;
        }
        const present = w32[ix32(at + S_PRESENT)] >>> 0, res = w32[ix32(at + S_BUFFER_LO)] >>> 0;
        const pid = w32[ix32(at + S_BUFFER_HI)] >>> 0, serial = w32[ix32(at + S_GPU_SERIAL)];
        if (Atomics.load(w32, ix32(at)) !== seq || (t.gpuPid && pid !== t.gpuPid) || watch.sent.get(i) === present
            || ((done - serial) | 0) < 0) {
          continue;
        }
        watch.sent.set(i, present);
        ex.grab(0, res, present);
      }
    };
    self.onmessage = (event) => {
      const m = event.data;
      if (m.type === "shown") {
        ex && ex.shown(m.serial);
        return;
      }
      if (m.type === "hidden") {
        watch.hidden = !!m.hidden;
        return;
      }
      if (m.type === "table") {
        // The window server's surface table (wlserver/README.md): watched from here every 2 ms, so that a GPU
        // window's new frame is blitted and on its way to the page as soon as the slot names it and the ring's
        // done has passed its gpu_serial, not only when display.js asks at its next frame.
        watch.table = m;
        if (!watch.timer) {
          watch.timer = setInterval(watchTable, 2);
        }
        return;
      }
      if (m.type === "grab") {
        // The page wants resource `res` as it is now, for a window's present `present` (stage B: the window
        // server's slot names the resource; the page asks once the ring's `done` has passed the frame).
        // (These two were lost when "vdec-port" was added, 661c4e0: GPU windows then never reached the page, and
        // a client whose buffers were never released, mpv, stood in eglSwapBuffers once all of its were busy.)
        if (ex && ex.grab) {
          ex.grab(m.ctx, m.res, m.present);
        }
        return;
      }
      if (m.type === "vdec-port") {
        // (the video decoder's Worker, tools/www/vdec-worker.js: frames to draw into guest surfaces)
        m.port.onmessage = (e) => {
          const f = e.data;
          if (f.type !== "vdec-frame") {
            return;
          }
          if (ex && ex.vdecFrame) {
            ex.vdecFrame(f);
          } else {
            // (the executor is not up yet: the frame is not drawn, the guest is told it is, not left waiting)
            f.frame.close();
            if (startMemory) {
              Atomics.store(new Int32Array(startMemory.buffer), Math.floor(f.ready / 4), f.seq | 0);
            }
          }
        };
        return;
      }
      if (m.type !== "start") {
        return;
      }
      startMemory = m.memory;
      canvas = new OffscreenCanvas(64, 64);
      gl = canvas.getContext("webgl2", { alpha: false, antialias: false, depth: false, stencil: false,
                                          preserveDrawingBuffer: false });
      if (m.exec === "vrend") {
        // vrend.js next to this file (gpu/vrend/build.sh's output), started on this context.
        const base = m.base;
        importScripts(base + "vrend/vrend.js");
        // (vrend's messages go to the page, whose console the drivers see; the first 200 of them)
        let said = 0;
        const say = (t) => {
          if (said++ < 200) {
            self.postMessage({ type: "log", text: "vrend: " + t });
          }
        };
        self.VrendModule({ locateFile: (f) => base + "vrend/" + f, print: say, printErr: say }).then((M) => {
          ex = new VrendExecutor(gl, m.memory, m.ring, M, m.caps);
          ex.grab = (ctx, res, present) => ex.onPresent(ctx, res, present, 0, ex.present(res), true);
          ex.onPresent = (ctx, res, frame, serial, t, grabbed) => {
            if (!t || !t.texture) {
              return;
            }
            const t0 = performance.now();
            if (canvas.width !== t.w || canvas.height !== t.h) {
              canvas.width = t.w;
              canvas.height = t.h;
            }
            ex.blitToCanvas(t);
            const bitmap = canvas.transferToImageBitmap();
            self.postMessage({ type: "present", ctx, res, frame, serial, bitmap, w: t.w, h: t.h, grabbed: !!grabbed, bgra: !!t.bgra,
                               blitMs: performance.now() - t0, stats: ex.stats }, [bitmap]);
          };
          run(ex);
          // (the counts to the page once a second: rvGpu.stats.exec)
          setInterval(() => self.postMessage({ type: "stats", stats: ex.stats }), 1000);
          self.postMessage({ type: "ready", renderer: renderer(gl), exec: "vrend", capsLen: ex.capsLen,
                             errors: ex.stats.errors });
        }, (error) => self.postMessage({ type: "ready", renderer: renderer(gl), exec: "vrend",
                                          errors: ["vrend.js: " + (error && error.message || error)] }));
        return;
      }
      ex = new Executor(gl, m.memory, m.ring);
      ex.grab = (ctx, res, present) => ex.onPresent(ctx, res, present, 0, ex.resources.get(res), true);
      ex.onPresent = (ctx, res, frame, serial, r, grabbed) => {
        if (!r || !r.fbo) {
          return;
        }
        const t0 = performance.now();
        if (canvas.width !== r.w || canvas.height !== r.h) {
          canvas.width = r.w;
          canvas.height = r.h;
        }
        // The resource into the canvas's own buffer (a blit on the GPU), the canvas as a bitmap, to the page.
        // (The blit turns it the right way up: a bitmap's first row is its top.)
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, r.fbo);
        gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
        gl.blitFramebuffer(0, 0, r.w, r.h, 0, 0, r.w, r.h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
        const bitmap = canvas.transferToImageBitmap();
        self.postMessage({ type: "present", ctx, res, frame, serial, bitmap, w: r.w, h: r.h, grabbed: !!grabbed,
                           blitMs: performance.now() - t0, stats: ex.stats }, [bitmap]);
      };
      run(ex);
      self.postMessage({ type: "ready", renderer: renderer(gl) });
    };
    return;
  }

  // ---- on the page
  const params = new URLSearchParams(location.search);
  // ?exec=vrend (virglrenderer in wasm: the real one; always in a Worker, since vrend keeps its own idea of
  // the GL state and display.js's drawing would upset it), ?exec=webgpu (vfront on WebGPU, rvgpu-worker.js)
  // or ?exec=proto (this file's own small decoder).
  // Without it: vrend for a machine's gpuserver, proto for gpu-demo.html's stub.
  const execParam = params.get("exec");
  let mode = params.get("gpu") || "main";
  const stats = { mode, presents: 0, imports: 0, importMs: 0, lastImportMs: 0, maxImportMs: 0, blitMs: 0,
                  exec: null, renderer: "" };
  const imported = new Map();	// res -> { texture, w, h } in display.js's context (worker mode)
  let ex = null, worker = null, started = null;
  let vdecPortHeld = null;	// the video decoder's port, for a Worker that is not there yet (rvGpu.vdecPort)
  const grabbing = new Map();	// res -> the present a grab was asked for
  let lastTable = null;
  const scriptSrc = document.currentScript ? document.currentScript.src : "gpu-exec.js";

  // A one-line note over the bottom edge of the page, outside the machine's display (the look of dev.html's
  // #netbanner, Catppuccin Mocha); also in stats.notice and through rvGpu.onNotice, for a page that shows it its own way.
  const notice = (text) => {
    stats.notice = text;
    if (rvGpu.onNotice) {
      rvGpu.onNotice(text);
      return;
    }
    try {
      const el = document.createElement("div");
      el.id = "gpunotice";
      el.setAttribute("role", "status");
      el.setAttribute("aria-live", "polite");
      el.textContent = text;
      el.style.cssText = "position:fixed;left:50%;bottom:36px;transform:translateX(-50%);z-index:40000;max-width:min(92vw,90ch);"
        + "box-sizing:border-box;padding:5px 12px;border-radius:6px;background:#1e1e2e;color:#cdd6f4;border:1px solid #45475a;"
        + "border-left:3px solid #f38ba8;font:12px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;"
        + "pointer-events:none;opacity:.94";
      document.body.appendChild(el);
    } catch (error) {
    }
  };

  const rvGpu = window.rvGpu = {
    stats,
    onPresent: null,
    onNotice: null,
    // Resource `res`'s texture in display.js's context, for a window's present `present` (stage B). On the
    // main thread it is the resource's own texture (present: whatever is asked); from a Worker it is the last
    // bitmap imported, and a newer one is asked for (it arrives a turn later; `present` says which it is).
    texture(pid, res, present = 0) {
      if (started && started.pid && pid && pid !== started.pid) {
        return null;	// (another GPU server's resource)
      }
      if (ex) {
        const t = ex.texture(res);
        return t ? Object.assign(t, { present }) : null;
      }
      const t = imported.get(res);
      if (worker && present && (!t || t.present !== present) && grabbing.get(res) !== present) {
        grabbing.set(res, present);
        worker.postMessage({ type: "grab", ctx: 0, res, present });
      }
      return t ? { texture: t.texture, flipY: false, w: t.w, h: t.h, present: t.present, bgra: t.bgra } : null;
    },
    // The ring's `done` now (display.js draws a GPU window's frame only once it has passed the slot's
    // gpu_serial), or null without a ring.
    done() {
      if (!started || !started.memory) {
        return null;
      }
      return Atomics.load(new Int32Array(started.memory.buffer), ix32(started.ring + H_DONE)) >>> 0;
    },
    shown(serial) {
      if (ex) {
        ex.shown(serial);
      } else if (worker) {
        worker.postMessage({ type: "shown", serial });
      }
    },
    // The video decoder's Worker wants frames drawn into guest surfaces (tools/www/vdec.js): its port goes to the
    // executor's Worker. Both Workers draw them (vrend's with WebGL, rvgpu-worker.js's with WebGPU: VK-14); false
    // when there is none (the guest's players then copy). A WebGPU executor that waits for the first GPU program
    // (VK-15, stats.lazy) has no Worker yet: the port is kept and goes to the Worker when it starts; the same
    // when the WebGPU Worker finds no device and vrend takes its place (the Worker hands the port back).
    vdecPort(port) {
      if (stats.lazy === "waiting" && !worker) {
        if (vdecPortHeld) {
          return false;
        }
        vdecPortHeld = port;
        return true;
      }
      if (mode !== "worker" || !worker || (stats.exec_kind !== "vrend" && stats.exec_kind !== "webgpu")) {
        return false;
      }
      worker.postMessage({ type: "vdec-port", port }, [port]);
      return true;
    },
    // The page is hidden, or shown again (display.js): the Worker sends no frames meanwhile. (The command
    // streams are carried out all the same: the guest's programs go on.)
    hidden(hidden) {
      stats.hidden = !!hidden;
      if (worker) {
        worker.postMessage({ type: "hidden", hidden: !!hidden });
      }
    },
    // Execute the ring at `ring` in `memory` (the machine's shared WebAssembly.Memory); gl: display.js's context.
    start(memory, ring, gl, pid = 0, exec = execParam || "proto", caps = null) {
      // ?exec=webgpu without WebGPU in this browser: vrend (WebGL2) on the same ring
      // (rvgpu.wasm is linked with JSPI: a browser with WebGPU but without JSPI cannot run it)
      const jspi = typeof WebAssembly.Suspending === "function" && typeof WebAssembly.promising === "function";
      if (exec === "webgpu" && (!navigator.gpu || !jspi)) {
        console.log("gpu-exec: no " + (navigator.gpu ? "JSPI (WebAssembly.Suspending)" : "WebGPU (navigator.gpu)") + " here: vrend instead");
        stats.fallback = navigator.gpu ? "no JSPI" : "no navigator.gpu";
        exec = "vrend";
      }
      stats.exec_kind = exec;
      if (exec === "vrend" || exec === "webgpu") {
        mode = stats.mode = "worker";
      }
      started = { ring, pid, memory };
      stats.renderer = renderer(gl);
      if (mode === "worker") {
        // ?exec=webgpu: gpu/webgpu's executor, rvgpu-worker.js (vfront on zink on wgvk, and vkr, on the page's
        // WebGPU device): the same ring and messages. GPU windows (slot format 2) come from both Workers as
        // ImageBitmaps; this side says when it has taken one ("imported"), so a Worker keeps one frame in flight
        // per resource. If the WebGPU Worker finds no device it says so before touching the ring ("fallback"),
        // and vrend's Worker starts on the ring in its place.
        const spawn = (kind) => {
          stats.exec_kind = kind;
          const w = new Worker(kind === "webgpu" ? scriptSrc.replace(/[^/]*$/, "") + "rvgpu-worker.js" : scriptSrc);
          w.onmessage = onWorker;
          // A WebGPU Worker that dies before it said "ready" (rvgpu.wasm did not load or compile, a script error):
          // vrend takes the ring in its place, as when it finds no device.
          w.onerror = (event) => {
            if (kind === "webgpu" && w === worker && stats.readyKind !== "webgpu" && !stats.deviceLost) {
              const why = "the WebGPU Worker failed: " + (event && event.message || "error");
              console.log("gpu-exec: " + why + ": vrend instead");
              stats.fallback = why;
              w.terminate();
              worker = spawn("vrend");
            }
          };
          w.postMessage({ type: "start", memory, ring, exec: kind, caps, base: scriptSrc.replace(/[^/]*$/, "") });
          if (vdecPortHeld) {
            w.postMessage({ type: "vdec-port", port: vdecPortHeld }, [vdecPortHeld]);
            vdecPortHeld = null;
          }
          if (lastTable) {
            w.postMessage({ type: "table", address: lastTable.address, slots: lastTable.slots,
                            slotSize: lastTable.slotSize, header: lastTable.header, gpuPid: pid });
          }
          if (document.hidden) {
            w.postMessage({ type: "hidden", hidden: true });
          }
          return w;
        };
        // The WebGPU device is lost mid-session (the GPU reset, the tab's device was destroyed, the driver crashed):
        // this executor can do nothing more. The ring says so (exec_state 2), and gpuserver then takes the ready
        // file away and writes rvgpu-0.none: programs started from now on draw in software at once. Programs that
        // are running keep their dead executor until the page is reloaded (their contexts lived in that device);
        // the notice says so. (rvgpu-worker.js logs "WebGPU device lost: REASON MESSAGE"; a later Worker may send
        // {type: "device-lost", reason, message} instead: both are taken.)
        const deviceLost = (text) => {
          if (stats.deviceLost || stats.exec_kind !== "webgpu") {
            return;
          }
          stats.deviceLost = text;
          console.log("gpu-exec: " + text + ": new programs draw in software");
          try {
            Atomics.store(new Int32Array(memory.buffer), ix32(ring + H_EXEC_STATE), 2);
          } catch (error) {
          }
          if (worker) {
            worker.terminate();
          }
          notice("GPU lost (" + text.replace(/^WebGPU device lost:\s*/, "") + "). New programs draw in software; "
                 + "programs that are open keep the lost GPU until you reload this page.");
        };
        const onWorker = (event) => {
          const m = event.data;
          if (m.type === "device-lost") {
            deviceLost("WebGPU device lost: " + (m.reason || "") + " " + (m.message || ""));
            return;
          }
          if (m.type === "log") {
            console.log(m.text);
            if (/^WebGPU device lost:/.test(m.text)) {
              deviceLost(m.text);
            }
            // (the first 40 kept, they say what went wrong first; then the last 30)
            if (!stats.vrendFirst || stats.vrendFirst.length < 40) {
              (stats.vrendFirst = stats.vrendFirst || []).push(m.text);
            } else {
              (stats.vrendLog = stats.vrendLog || []).push(m.text);
              if (stats.vrendLog.length > 30) {
                stats.vrendLog.shift();
              }
            }
            return;
          }
          if (m.type === "ready" && m.fallback) {
            console.log("gpu-exec: the WebGPU Worker found no device (" + (m.errors || []).join("; ") + "): vrend instead");
            stats.fallback = (m.errors || []).join("; ") || "no WebGPU device";
            worker.terminate();
            vdecPortHeld = m.vdecPort || vdecPortHeld;	// (the video decoder's port, handed back: vrend draws its frames)
            worker = spawn("vrend");
            return;
          }
          if (m.type === "ready") {
            stats.readyKind = m.exec || stats.exec_kind;
            if (stats.lazyWantAt) {
              // (VK-15: from the first program's connection to the executor's caps: fetch + compile + start)
              stats.lazyDelayMs = Math.round(performance.now() - stats.lazyWantAt);
              console.log("gpu-exec: the executor was started by the first program and took " + stats.lazyDelayMs + " ms to answer");
            }
            stats.workerRenderer = m.renderer;
            stats.capsLen = m.capsLen;
            stats.startErrors = m.errors || [];
            return;
          }
          if (m.type === "stats") {
            stats.exec = m.stats;
            return;
          }
          if (m.type === "debug") {
            // (rvgpu-worker.js: the module's memory and where its executor says what it is doing; read here
            // when the executor's heartbeat stops, since the Worker then cannot say anything itself)
            let beat = -1, still = 0, said = 0;
            setInterval(() => {
              const b = Atomics.load(new Int32Array(memory.buffer), ix32(ring + 72));
              const t = performance.now();
              if (b !== beat) {
                beat = b;
                still = t;
                return;
              }
              if (t - still > 2000 && t - said > 5000) {
                said = t;
                const heap = new Uint8Array(m.heap);
                let end = m.where;
                while (end < m.where + 128 && heap[end]) end++;
                const where = new TextDecoder().decode(heap.slice(m.where, end));
                const w = new Uint32Array(m.heap, m.words, 8);
                console.log("rvgpu: the executor has not moved for " + Math.round(t - still) + " ms: " + (where || "-")
                            + "; venus command " + (w[0] ? w[0] - 1 : "none") + " (" + ["", "ring", "stream", "context"][w[1]]
                            + "), " + w[3] + " run; words " + Array.from(w).join(","));
              }
            }, 1000);
            return;
          }
          if (m.type !== "present") {
            return;
          }
          if (m.grabbed && grabbing.get(m.res) === m.frame) {
            grabbing.delete(m.res);
          }
          // The bitmap into this context's texture for that resource: GPU memory to GPU memory.
          const t0 = performance.now();
          let t = imported.get(m.res);
          if (!t) {
            t = { texture: gl.createTexture(), w: 0, h: 0 };
            gl.bindTexture(gl.TEXTURE_2D, t.texture);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
            imported.set(m.res, t);
          }
          gl.bindTexture(gl.TEXTURE_2D, t.texture);
          gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
          gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
          gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, m.bitmap);
          m.bitmap.close();
          t.w = m.w;
          t.h = m.h;
          t.present = m.frame;
          t.bgra = !!m.bgra;
          worker.postMessage({ type: "imported", res: m.res, frame: m.frame });
          const dt = performance.now() - t0;
          stats.imports++;
          stats.importMs += dt;
          stats.lastImportMs = dt;
          stats.maxImportMs = Math.max(stats.maxImportMs, dt);
          stats.blitMs += m.blitMs;
          stats.exec = m.stats;
          stats.presents++;
          if (!m.grabbed && rvGpu.onPresent) {
            rvGpu.onPresent(m.ctx, m.res, m.frame, m.serial);
          }
        };
        worker = spawn(exec);
        // (the window server's table, for the Worker to watch GPU windows' frames itself)
        window.rvChannel("rv-display").onmessage = (event) => {
          const t = event.data;
          if (t && t.type === "table" && (!lastTable || lastTable.address !== t.address || lastTable.pid !== t.pid)) {
            lastTable = t;
            worker.postMessage({ type: "table", address: t.address, slots: t.slots, slotSize: t.slotSize,
                                 header: t.header, gpuPid: pid });
          }
        };
        return;
      }
      ex = new Executor(gl, memory, ring);
      stats.exec = ex.stats;
      ex.onPresent = (ctx, res, frame, serial) => {
        stats.presents++;
        if (rvGpu.onPresent) {
          rvGpu.onPresent(ctx, res, frame, serial);
        }
      };
      run(ex);
    },
  };

  // The choice, made once per page (the first announcement of the ring asks, the others share the answer): ?exec= wins and
  // needs nothing else; otherwise rv-site.json's gpu_exec, and the adapter probe only when that says webgpu or auto.
  let chosen = null;
  const chooseExec = (gl) => chosen || (chosen = (async () => {
    const override = execParam ? execParam.toLowerCase() : "";
    const decided = override === "vrend" || override === "webgpu" || override === "proto";
    let site = null, probe = null;
    if (!decided) {
      try {
        const answer = await fetch("rv-site.json", { cache: "no-store", signal: AbortSignal.timeout(3000) });
        site = answer.ok ? (await answer.json()).gpu_exec : null;
      } catch (error) {
        // (no file, no answer: vrend)
      }
      if (override === "auto") {
        site = "auto";
      }
      if (normExec(site) === "webgpu" || normExec(site) === "auto") {
        probe = await probeAdapter(navigator.gpu);
      }
    }
    const choice = selectExec({ override, site, hasGpu: !!navigator.gpu, hasGL2: !!gl, browserOk: browserAllowed(navigator), probe });
    stats.select = choice;
    console.log("gpu-exec: " + choice.exec + " (" + choice.why + ")");
    return choice;
  })());

  // gpuserver's announcement (PROTOCOL.md): start on the machine's memory, in display.js's context.
  const gpuChannel = window.rvChannel("rv-gpu");
  // (Asked again every 2 s until a ring is taken: gpuserver answers a "gpu?" at once.)
  const ask = setInterval(() => (started ? clearInterval(ask) : gpuChannel.postMessage({ type: "gpu?" })), 2000);
  gpuChannel.onmessage = (event) => {
    const m = event.data;
    const os = window.linuxOs, d = window.rvDisplay;
    // (?gpu=off: no executor; the machine's programs then draw in software, as on a page without a GPU)
    if (mode === "off" || !m || m.type !== "gpu" || (m.version !== 1 && m.version !== 2) || started || !os || !os.memory || !d || !d.gl) {
      return;
    }
    chooseExec(d.gl).then((choice) => {
      if (started || choice.exec === "none") {
        return;
      }
      // The executor is chosen once (chooseExec: ?exec=, rv-site.json gpu_exec, the probe), then:
      // VK-15: the WebGPU executor (rvgpu.wasm, 16 MB of wasm to fetch and compile, and its code in the tab's 4 GiB of
      // compiled wasm) is started when the first program connects to gpuserver, not at boot. gpuserver (`lazy` in its
      // announcement; an older one does not say it) is told by exec_state 4 that an executor is promised, and answers
      // with `want` on the first connection. ?rvgpu=eager starts it at once as before; without navigator.gpu the
      // fallback to vrend starts at once (that is not deferred).
      // (VK-13: "the WebGPU executor" is the one the selection chose, not only ?exec=webgpu.)
      if (!m.want && m.lazy && choice.exec === "webgpu" && navigator.gpu && params.get("rvgpu") !== "eager") {
        if (!stats.lazy) {
          stats.lazy = "waiting";
          console.log("gpu-exec: the WebGPU executor waits for the first GPU program (?rvgpu=eager: at boot)");
        }
        Atomics.store(new Int32Array(os.memory.buffer), ix32(m.ring + 76), 4);	// exec_state 4: promised, not started
        return;
      }
      if (stats.lazy === "waiting") {
        stats.lazy = "started";
        stats.lazyWantAt = performance.now();
        console.log("gpu-exec: the first GPU program has connected: starting the WebGPU executor");
      }
      rvGpu.start(os.memory, m.ring, d.gl, m.pid, choice.exec, m.caps ? { at: m.caps, size: m.capsSize } : null);
    });
  };
})();
