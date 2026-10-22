// The page's video decoder for guest players (docs/video-decode.md): a dedicated Worker that reads gpuserver's
// vdec ring (gpu/server/vdec.js has its layout) out of the machine's memory, decodes the chunks with WebCodecs'
// VideoDecoder (on the Mac: VideoToolbox, the hardware decoder) and keeps each surface's newest frame:
//   - copy: VideoFrame.copyTo() straight into the guest's image (an NV12 memory file), for vaGetImage;
//   - zero copy: the frame (a handle, transferred, not its pixels) to the GPU executor's Worker over a
//     MessagePort, which draws it into the surface's two GPU resources (tools/www/gpu-exec.js, vdecFrame).
// What has been done goes into the client's status file, which the guest's driver reads: decoded, drawn,
// copied, chunk bytes taken.
//
// The core (VdecCore) takes its WebCodecs from outside, so that gpu/tools/vdec-stub.js can run it under
// tools/node-machine.js with a stand-in decoder (Node has no WebCodecs).
(function (root) {
  "use strict";

  const H = { MAGIC: 0, VERSION: 4, PID: 8, HEARTBEAT: 12, DATA_SIZE: 16, DATA_OFF: 20, HEAD: 24, DOORBELL: 28,
    TAIL: 64, PAGE_STATE: 68, PAGE_BEAT: 72, CAPS_N: 76, PAGE_FLAGS: 80, CAPS: 1024 };
  const OP = { PAD: 0, OPEN: 1, CLOSE: 2, MEM: 3, MEM_END: 4, STREAM: 5, STREAM_END: 6, SURFACE: 7, DECODE: 8, COPY: 9,
    EXPORT: 10, SLOT_END: 11 };
  const ST = { BEAT: 4, STATE: 8, CONSUMED: 12, SLOTS: 4096, SLOT: 32 };	// slot: decoded ready copied copy_status error
  // A byte address of the machine's memory as an index of its words: addresses go past 2 GiB (and past 4 GiB with
  // ?mem= above that), where `>> 2` would make them negative or cut them.
  const ix = (a) => Math.floor(a / 4);
  const CODEC = { H264: 1, HEVC: 2, VP8: 3, VP9: 4, AV1: 5 };
  const CHUNK = { KEY: 1, HIDDEN: 2, DRAW: 4 };
  const NAMES = { 1: "H.264", 2: "HEVC", 3: "VP8", 4: "VP9", 5: "AV1" };

  // What is asked of the browser at the start (isConfigSupported), per codec: a codec string and the sizes,
  // the largest first.
  const PROBES = [
    { codec: CODEC.H264, strings: ["avc1.640033", "avc1.640033", "avc1.64002a"] },
    { codec: CODEC.HEVC, strings: ["hev1.1.6.L156.B0", "hev1.1.6.L153.B0", "hev1.1.6.L123.B0"] },
    { codec: CODEC.VP8, strings: ["vp8", "vp8", "vp8"] },
    { codec: CODEC.VP9, strings: ["vp09.00.51.08", "vp09.00.51.08", "vp09.00.41.08"] },
    { codec: CODEC.AV1, strings: ["av01.0.13M.08", "av01.0.12M.08", "av01.0.08M.08"] },
  ];
  const SIZES = [[4096, 2304], [3840, 2160], [1920, 1080]];

  const hex2 = (n) => (n & 255).toString(16).toUpperCase().padStart(2, "0");
  // The codec string for a stream (profile and level as the guest's driver made its parameter sets)
  const codecString = (codec, profile, level, width, height) => {
    switch (codec) {
      case CODEC.H264: return "avc1." + hex2(profile || 100) + "00" + hex2(level || 51);
      case CODEC.HEVC: return "hev1.1.6.L" + (level || 153) + ".B0";
      case CODEC.VP8: return "vp8";
      case CODEC.VP9: return "vp09.00." + String(level || (width * height > 2228224 ? 51 : 41)).padStart(2, "0") + ".08";
      case CODEC.AV1: return "av01.0." + String(level || (width * height > 2228224 ? 12 : 8)).padStart(2, "0") + "M.08";
    }
    return "";
  };

  // Ask the browser what it decodes: [{codec, profile, max_w, max_h, flags (1 hardware, 2 software)}].
  const probeCaps = async (VideoDecoder, only) => {
    const caps = [];
    for (const p of PROBES) {
      if (only && !only.includes(p.codec)) {
        continue;
      }
      let best = null;
      for (const pref of ["prefer-hardware", "no-preference"]) {
        for (let i = 0; i < SIZES.length && !best; i++) {
          try {
            const r = await VideoDecoder.isConfigSupported({ codec: p.strings[i], codedWidth: SIZES[i][0],
              codedHeight: SIZES[i][1], hardwareAcceleration: pref });
            if (r && r.supported) {
              best = { codec: p.codec, profile: 0, max_w: SIZES[i][0], max_h: SIZES[i][1], flags: pref === "prefer-hardware" ? 1 : 2 };
            }
          } catch (error) {
            // (a codec string the browser does not know: not supported)
          }
        }
        if (best) {
          break;
        }
      }
      if (best) {
        caps.push(best);
      }
    }
    return caps;
  };

  // ---- the core
  class VdecCore {
    // env: { memory, ring, VideoDecoder, EncodedVideoChunk, gpuPort (or null), log, maxFrameBytes, exact }
    constructor(env) {
      this.env = env;
      this.memory = env.memory;
      this.ring = env.ring;
      this.log = env.log || ((t) => console.log(t));
      this.clients = new Map();
      this.held = [];	// slots with frames, oldest first: { client, slot }
      this.heldBytes = 0;
      this.maxFrameBytes = env.maxFrameBytes || 384 * 1024 * 1024;
      this.sharedCopies = true;	// copyTo straight into the machine's memory (else through a buffer)
      this.stats = { chunks: 0, bytes: 0, frames: 0, copies: 0, draws: 0, errors: 0, keys: 0, dropped: 0,
        decodeMs: 0, copyMs: 0, streams: 0, lastError: "" };
      this.beat = 0;
    }
    w32() {
      return new Int32Array(this.memory.buffer);
    }
    hw(off) {
      return Atomics.load(this.w32(), ix(this.ring + off)) >>> 0;
    }
    setHw(off, v) {
      Atomics.store(this.w32(), ix(this.ring + off), v | 0);
    }
    // The page's caps into the ring's header, then "ready" (gpuserver answers HELLO from them).
    publish(caps, gpu) {
      const v = new DataView(this.memory.buffer);
      caps.slice(0, 32).forEach((c, i) => [c.codec, c.profile, c.max_w, c.max_h, c.flags]
        .forEach((x, k) => v.setUint32(this.ring + H.CAPS + 20 * i + 4 * k, x >>> 0, true)));
      this.setHw(H.CAPS_N, Math.min(caps.length, 32));
      this.setHw(H.PAGE_FLAGS, gpu ? 1 : 0);
      this.setHw(H.PAGE_STATE, caps.length ? 1 : 3);
      this.caps = caps;
    }
    failed() {
      this.setHw(H.PAGE_STATE, 3);
    }
    // ---- status words of a client
    st(c, off, v) {
      Atomics.store(this.w32(), ix(c.status + off), v | 0);
    }
    stSlot(c, slot, i, v) {
      this.st(c, ST.SLOTS + ST.SLOT * slot + 4 * i, v);
    }

    // One turn: the records there are, the beat, the overdue chunks.
    turn() {
      const w = this.w32(), r = this.ring;
      if ((Atomics.load(w, ix(r + H.MAGIC)) >>> 0) !== 0x44565652) {
        return;
      }
      const size = this.hw(H.DATA_SIZE), data = r + this.hw(H.DATA_OFF);
      for (;;) {
        const head = this.hw(H.HEAD), tail = this.hw(H.TAIL);
        if (head === tail) {
          break;
        }
        const at = tail % size, p = ix(data + at);
        const op = w[p], client = w[p + 1] >>> 0, bytes = w[p + 3] >>> 0;
        const used = 16 + ((bytes + 15) & ~15);
        if (op !== OP.PAD) {
          try {
            this.record(op, client, Array.from(w.subarray(p + 4, p + 4 + (bytes >> 2)), (x) => x >>> 0));
          } catch (error) {
            this.stats.errors++;
            const cl = this.clients.get(client);
            this.stats.lastError = "record " + op + ": " + (error && error.message || error)
              + (cl ? " (client " + client + ": status file at 0x" + cl.status.toString(16) + ", memory 0x"
                 + this.memory.buffer.byteLength.toString(16) + ")" : "");
            this.log("vdec: " + this.stats.lastError);
          }
        }
        this.setHw(H.TAIL, (tail + used) >>> 0);
      }
      this.beat++;
      this.setHw(H.PAGE_BEAT, this.beat);
      const t = performance.now();
      for (const c of this.clients.values()) {
        this.st(c, ST.BEAT, this.beat);
        for (const s of c.streams.values()) {
          for (const [seq, p] of s.pending) {
            if (t - p.t > 8000) {
              this.log("vdec: client " + c.id + " stream " + s.id + ": chunk " + seq + " gave no frame in 8 s");
              s.pending.delete(seq);
              this.stSlot(c, p.slot, 4, seq);
              this.stats.dropped++;
            }
          }
        }
      }
    }

    record(op, id, a) {
      const lo = (i) => a[i] + a[i + 1] * 0x100000000;
      if (op === OP.OPEN) {
        this.clients.set(id, { id, status: lo(0), streams: new Map(), slots: new Map(), copies: new Map() });
        return;
      }
      const c = this.clients.get(id);
      if (!c) {
        return;
      }
      switch (op) {
        case OP.CLOSE:
          for (const s of c.streams.values()) {
            this.streamEnd(c, s);
          }
          for (const slot of [...c.slots.keys()]) {
            this.slotEnd(c, slot);
          }
          this.clients.delete(id);
          break;
        case OP.MEM:
        case OP.MEM_END:
          break;
        case OP.STREAM:
          this.stream(c, a[0], a[1], a[2], a[3], a[4], a[5]);
          break;
        case OP.STREAM_END: {
          const s = c.streams.get(a[0]);
          if (s) {
            this.streamEnd(c, s);
          }
          break;
        }
        case OP.SURFACE: {
          const sl = this.slot(c, a[0]);
          sl.w = a[1];
          sl.h = a[2];
          sl.res = [a[3], a[4]];
          break;
        }
        case OP.SLOT_END:
          this.slotEnd(c, a[0]);
          break;
        case OP.DECODE:
          this.decode(c, a[0], a[1], a[2], lo(3), a[5], a[6], a[7]);
          break;
        case OP.COPY:
          this.copy(c, { slot: a[0], id: a[1], seq: a[2], addr: lo(3), offY: a[5], pitchY: a[6], offUV: a[7],
            pitchUV: a[8], width: a[9], height: a[10], size: a[11] });
          break;
        case OP.EXPORT: {
          const sl = this.slot(c, a[0]);
          sl.gpu = true;
          if (sl.frame) {
            this.draw(c, a[0], sl);
          } else {
            this.stSlot(c, a[0], 1, sl.seq);
          }
          break;
        }
      }
    }

    slot(c, n) {
      let sl = c.slots.get(n);
      if (!sl) {
        sl = { frame: null, seq: 0, bytes: 0, gpu: false, res: [0, 0], w: 0, h: 0 };
        c.slots.set(n, sl);
      }
      return sl;
    }
    slotEnd(c, n) {
      const sl = c.slots.get(n);
      if (sl) {
        this.drop(sl);
        c.slots.delete(n);
      }
      c.copies.delete(n);
    }
    drop(sl) {
      if (sl.frame) {
        sl.frame.close();
        sl.frame = null;
        this.heldBytes -= sl.bytes;
        this.held = this.held.filter((h) => h !== sl);
      }
    }

    // ---- streams
    stream(c, id, codec, profile, level, width, height) {
      const old = c.streams.get(id);
      if (old) {
        this.streamEnd(c, old);
      }
      const cap = (this.caps || []).find((x) => x.codec === codec);
      const s = { id, codec, decoder: null, pending: new Map(), needKey: true, closed: false,
        config: { codec: codecString(codec, profile, level, width, height), codedWidth: width, codedHeight: height,
          hardwareAcceleration: cap && cap.flags & 1 ? "prefer-hardware" : "no-preference", optimizeForLatency: true,
          colorSpace: { primaries: "bt709", transfer: "bt709", matrix: "bt709", fullRange: true } } };
      c.streams.set(id, s);
      this.stats.streams++;
      this.open(c, s);
      this.log("vdec: client " + c.id + " stream " + id + ": " + (NAMES[codec] || codec) + " " + width + "x" + height
               + " as " + s.config.codec + " (" + s.config.hardwareAcceleration + ")");
    }
    open(c, s) {
      const { VideoDecoder } = this.env;
      s.decoder = new VideoDecoder({
        output: (frame) => this.output(c, s, frame),
        error: (e) => this.failure(c, s, e),
      });
      s.decoder.configure(s.config);
      s.needKey = true;
    }
    streamEnd(c, s) {
      s.closed = true;
      if (s.decoder && s.decoder.state !== "closed") {
        s.decoder.close();
      }
      s.pending.clear();
      c.streams.delete(s.id);
    }
    // The decoder failed (WebCodecs closes it): every chunk in it failed; a new one waits for a key chunk.
    failure(c, s, e) {
      this.stats.errors++;
      this.stats.lastError = (NAMES[s.codec] || s.codec) + ": " + (e && e.message || e);
      this.log("vdec: client " + c.id + " stream " + s.id + ": " + this.stats.lastError);
      for (const [seq, p] of s.pending) {
        this.stSlot(c, p.slot, 4, seq);
      }
      s.pending.clear();
      if (!s.closed) {
        this.open(c, s);
      }
    }

    decode(c, id, slot, seq, addr, size, flags, end) {
      const s = c.streams.get(id);
      const data = new Uint8Array(this.memory.buffer, addr, size).slice();
      this.st(c, ST.CONSUMED, end);
      this.stats.chunks++;
      this.stats.bytes += size;
      if (!s) {
        this.stSlot(c, slot, 4, seq);
        return;
      }
      if (s.needKey && !(flags & CHUNK.KEY)) {
        // (WebCodecs needs a key chunk after configure(): the picture fails, as on a seek into a GOP)
        if (!(flags & CHUNK.HIDDEN)) {
          this.stSlot(c, slot, 4, seq);
        }
        this.stats.dropped++;
        return;
      }
      if (s.decoder.state === "closed") {
        this.open(c, s);
        if (!(flags & CHUNK.KEY)) {
          this.stSlot(c, slot, 4, seq);
          return;
        }
      }
      s.needKey = false;
      if (flags & CHUNK.KEY) {
        this.stats.keys++;
      }
      if (!(flags & CHUNK.HIDDEN)) {
        s.pending.set(seq, { slot, flags, t: performance.now() });
      }
      const chunk = new this.env.EncodedVideoChunk({ type: flags & CHUNK.KEY ? "key" : "delta", timestamp: seq, data });
      try {
        s.decoder.decode(chunk);
      } catch (error) {
        this.failure(c, s, error);
      }
    }

    output(c, s, frame) {
      const seq = Number(frame.timestamp) >>> 0, p = s.pending.get(seq);
      if (!p) {
        frame.close();	// (a frame of a chunk that already failed, or of a stream that ended)
        return;
      }
      s.pending.delete(seq);
      this.stats.frames++;
      this.stats.decodeMs += performance.now() - p.t;
      const sl = this.slot(c, p.slot);
      this.drop(sl);
      sl.frame = frame;
      sl.seq = seq;
      sl.bytes = frame.codedWidth * frame.codedHeight * 3 / 2;
      this.heldBytes += sl.bytes;
      this.held.push(sl);
      // (frames hold GPU memory: the oldest go when there are too many; a copy of one fails then)
      while (this.heldBytes > this.maxFrameBytes && this.held.length > 4) {
        this.drop(this.held[0]);
      }
      this.stSlot(c, p.slot, 0, seq);
      if ((p.flags & CHUNK.DRAW) || sl.gpu) {
        sl.gpu = true;
        this.draw(c, p.slot, sl);
      }
      const q = c.copies.get(p.slot);
      if (q && q.seq === seq) {
        c.copies.delete(p.slot);
        this.copy(c, q);
      }
    }

    // ---- zero copy: the frame to the executor, which draws it into the slot's resources and says "ready"
    draw(c, n, sl) {
      const port = this.env.gpuPort;
      const ready = c.status + ST.SLOTS + ST.SLOT * n + 4;
      if (!port || !sl.res[0] || !sl.frame) {
        Atomics.store(this.w32(), ix(ready), sl.seq | 0);	// (nothing to draw into: say so, not wait)
        return;
      }
      const frame = sl.frame.clone();
      this.stats.draws++;
      port.postMessage({ type: "vdec-frame", frame, res: sl.res, w: sl.w, h: sl.h, ready, seq: sl.seq,
        exact: !!this.env.exact }, [frame]);
    }

    // ---- copies (vaGetImage): the frame as NV12 into the image
    copy(c, q) {
      const sl = c.slots.get(q.slot);
      if (!sl || !sl.frame || sl.seq !== q.seq) {
        const s = [...c.streams.values()].find((st) => [...st.pending.values()].some((p) => p.slot === q.slot));
        if (s) {
          c.copies.set(q.slot, q);	// (its frame is on the way)
          return;
        }
        this.stSlot(c, q.slot, 3, 1);
        this.stSlot(c, q.slot, 2, q.id);
        return;
      }
      const t0 = performance.now(), frame = sl.frame;
      this.copyFrame(frame, q).then(() => {
        this.stats.copies++;
        this.stats.copyMs += performance.now() - t0;
        this.stSlot(c, q.slot, 3, 0);
        this.stSlot(c, q.slot, 2, q.id);
      }, (error) => {
        this.stats.errors++;
        this.stats.lastError = "copy: " + (error && error.message || error);
        this.log("vdec: " + this.stats.lastError);
        this.stSlot(c, q.slot, 3, 2);
        this.stSlot(c, q.slot, 2, q.id);
      });
    }
    async copyFrame(frame, q) {
      const w = Math.min(q.width, frame.codedWidth) & ~1, h = Math.min(q.height, frame.codedHeight) & ~1;
      const rect = { x: 0, y: 0, width: w, height: h };
      const view = () => new Uint8Array(this.memory.buffer, q.addr, q.size);
      if (frame.format === "NV12") {
        const layout = [{ offset: q.offY, stride: q.pitchY }, { offset: q.offUV, stride: q.pitchUV }];
        if (this.sharedCopies) {
          try {
            await frame.copyTo(view(), { rect, layout });
            return;
          } catch (error) {
            if (!(error instanceof TypeError)) {
              throw error;
            }
            this.sharedCopies = false;
            this.log("vdec: copyTo does not take the machine's (shared) memory: through a buffer");
          }
        }
        const tmp = new Uint8Array(q.size);
        await frame.copyTo(tmp, { rect, layout });
        view().set(tmp.subarray(0, q.offUV + q.pitchUV * (h / 2)));
        return;
      }
      if (frame.format === "I420" || frame.format === "I420A") {
        // (software decoders give I420: the planes into a buffer, then Y as it is and U, V interleaved)
        const tmp = new Uint8Array(w * h * 3 / 2), cw = w / 2;
        await frame.copyTo(tmp, { rect, layout: [{ offset: 0, stride: w }, { offset: w * h, stride: cw },
          { offset: w * h + cw * (h / 2), stride: cw }] });
        const out = view();
        for (let y = 0; y < h; y++) {
          out.set(tmp.subarray(y * w, y * w + w), q.offY + y * q.pitchY);
        }
        const u = w * h, v = u + cw * (h / 2);
        for (let y = 0; y < h / 2; y++) {
          const row = q.offUV + y * q.pitchUV;
          for (let x = 0; x < cw; x++) {
            out[row + 2 * x] = tmp[u + y * cw + x];
            out[row + 2 * x + 1] = tmp[v + y * cw + x];
          }
        }
        return;
      }
      throw new Error("frames of format " + frame.format + " cannot be copied as NV12");
    }
  }

  // Turns: on every doorbell (Atomics.waitAsync, so that the decoder's callbacks run between), and every 100 ms.
  const run = (core) => {
    const w = core.w32(), bell = ix(core.ring + H.DOORBELL);
    const wait = () => {
      const seen = Atomics.load(core.w32(), bell);
      core.turn();
      if (typeof Atomics.waitAsync !== "function") {
        // (Firefox before 145: no Atomics.waitAsync; look again in 2 ms, a doorbell waits that long at most)
        setTimeout(wait, 2);
        return;
      }
      const r = Atomics.waitAsync(core.w32(), bell, seen, 100);
      (r.async ? r.value : Promise.resolve()).then(wait);
    };
    void w;
    wait();
  };

  const api = { VdecCore, probeCaps, codecString, run, H, OP, ST, CODEC, CHUNK };
  if (typeof module !== "undefined" && module.exports) {
    module.exports = api;
    return;
  }
  root.RvVdec = api;
  // ---- in its Worker (tools/www/vdec.js starts it): {type: "start", memory, ring, port, exact, frames}
  if (typeof WorkerGlobalScope !== "undefined" && root instanceof WorkerGlobalScope) {
    let core = null;
    root.onmessage = async (event) => {
      const m = event.data;
      if (m.type !== "start" || core) {
        return;
      }
      const say = (t) => root.postMessage({ type: "log", text: t });
      core = new VdecCore({ memory: m.memory, ring: m.ring, VideoDecoder: root.VideoDecoder,
        EncodedVideoChunk: root.EncodedVideoChunk, gpuPort: m.port || null, log: say, exact: m.exact,
        maxFrameBytes: m.frames ? m.frames * 1024 * 1024 : 0 });
      if (typeof root.VideoDecoder === "undefined") {
        say("vdec: no WebCodecs VideoDecoder in this browser: guest players decode in software");
        core.failed();
        root.postMessage({ type: "ready", caps: [] });
        return;
      }
      const t0 = performance.now();
      const caps = await probeCaps(root.VideoDecoder, m.only);
      core.publish(caps, !!m.port);
      root.postMessage({ type: "ready", caps, ms: Math.round(performance.now() - t0), gpu: !!m.port });
      run(core);
      setInterval(() => root.postMessage({ type: "stats", stats: core.stats, held: core.held.length,
        heldMiB: Math.round(core.heldBytes / 1048576) }), 1000);
    };
  }
})(typeof self !== "undefined" ? self : this);
