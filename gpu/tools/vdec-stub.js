// The page's video decoder under tools/node-machine.js, with a stand-in for WebCodecs (Node has none): the real
// tools/www/vdec-worker.js core reads gpuserver's vdec ring and writes the status words, but its VideoDecoder
// decodes nothing: each chunk gives a frame (its timestamp passed through, as VideoDecoder does) whose pixels
// are a pattern of the chunk's number, copied into the guest's images like a real frame. What the guest sent is
// written to VDEC_STUB_DUMP (a directory) per stream, in the driver's dump format, for gpu/vdec/test/chunks.py
// and ffmpeg to decode on their own.
//   HOOK=gpu/tools/gpu-stub.js GPU_STUB_CHAIN=gpu/tools/vdec-stub.js gpu/tools/nm-run.sh '...'
//   VDEC_STUB_GPU=1     say frames can be drawn into GPU resources (they are only marked drawn)
//   VDEC_STUB_CODECS=1,4  the codecs offered (default all: 1 H.264, 2 HEVC, 3 VP8, 4 VP9, 5 AV1)
//   VDEC_STUB_DUMP=DIR  the chunks
//   VDEC_STUB_CHAIN=tools/wl-dump.js  also load that hook (wl-dump acknowledges the window server's frames, so that
//                       a GL player's buffers are given back)
"use strict";

const fs = require("fs");
const path = require("path");

module.exports = (os, opts) => {
  const chained = process.env.VDEC_STUB_CHAIN ? require(path.resolve(process.env.VDEC_STUB_CHAIN))(os, opts) : null;
  const { VdecCore, run, probeCaps } = require(path.join(__dirname, "../../tools/www/vdec-worker.js"));
  const dump = process.env.VDEC_STUB_DUMP;
  const gpu = process.env.VDEC_STUB_GPU === "1";
  const codecs = (process.env.VDEC_STUB_CODECS || "1,2,3,4,5").split(",").map(Number);
  const CODEC_OF = { avc1: 1, hev1: 2, vp8: 3, vp09: 4, av01: 5 };
  const files = new Map();
  if (dump) {
    fs.mkdirSync(dump, { recursive: true });
  }

  class Frame {
    constructor(timestamp, w, h) {
      this.timestamp = timestamp;
      this.codedWidth = this.displayWidth = w;
      this.codedHeight = this.displayHeight = h;
      this.visibleRect = { x: 0, y: 0, width: w, height: h };
      this.format = "NV12";
      this.colorSpace = { primaries: "bt709", transfer: "bt709", matrix: "bt709", fullRange: true };
      this.closed = false;
    }
    clone() {
      return new Frame(this.timestamp, this.codedWidth, this.codedHeight);
    }
    close() {
      this.closed = true;
    }
    async copyTo(dest, { rect, layout }) {
      if (this.closed) {
        throw new Error("InvalidStateError: closed");
      }
      const t = this.timestamp & 255;
      for (let y = 0; y < rect.height; y++) {
        dest.fill((t + y) & 255, layout[0].offset + y * layout[0].stride, layout[0].offset + y * layout[0].stride + rect.width);
      }
      for (let y = 0; y < rect.height / 2; y++) {
        dest.fill(128, layout[1].offset + y * layout[1].stride, layout[1].offset + y * layout[1].stride + rect.width);
      }
      return layout;
    }
  }
  let decoders = 0;
  class VideoDecoder {
    static async isConfigSupported(config) {
      const codec = CODEC_OF[config.codec.split(".")[0]];
      return { supported: codecs.includes(codec), config };
    }
    constructor({ output, error }) {
      this.output = output;
      this.error = error;
      this.state = "unconfigured";
      this.id = ++decoders;
    }
    configure(config) {
      this.config = config;
      this.state = "configured";
    }
    decode(chunk) {
      if (this.state !== "configured") {
        throw new Error("InvalidStateError: " + this.state);
      }
      const f = new Frame(chunk.timestamp, this.config.codedWidth, this.config.codedHeight);
      setImmediate(() => this.state === "configured" && this.output(f));
    }
    close() {
      this.state = "closed";
    }
  }
  class EncodedVideoChunk {
    constructor({ type, timestamp, data }) {
      this.type = type;
      this.timestamp = timestamp;
      this.data = new Uint8Array(data);
      this.byteLength = this.data.length;
    }
  }
  const port = gpu ? {
    postMessage(m) {
      m.frame.close();
      Atomics.store(new Int32Array(os.memory.buffer), Math.floor(m.ready / 4), m.seq | 0);
    },
  } : null;

  let core = null;
  const channel = new BroadcastChannel("rv-gpu");
  const ask = setInterval(() => channel.postMessage({ type: "gpu?" }), 1000);
  channel.onmessage = async (e) => {
    const m = e.data;
    if (!m || m.type !== "gpu" || !m.vdec || core) {
      return;
    }
    core = new VdecCore({ memory: os.memory, ring: m.vdec, VideoDecoder, EncodedVideoChunk, gpuPort: port,
      log: (t) => console.error("vdec-stub: " + t) });
    // (what the guest sent, for decoding it elsewhere)
    const decode = core.decode.bind(core);
    core.decode = (c, id, slot, seq, addr, size, flags, end) => {
      if (dump) {
        const s = c.streams.get(id);
        const key = "c" + c.id + "-s" + id;
        if (!files.has(key)) {
          files.set(key, fs.openSync(path.join(dump, key + ".chunks"), "w"));
        }
        const h = Buffer.alloc(16);
        h.writeUInt32LE(id, 0);
        h.writeUInt32LE(s ? s.codec : 0, 4);
        h.writeUInt32LE(flags, 8);
        h.writeUInt32LE(size, 12);
        fs.writeSync(files.get(key), h);
        fs.writeSync(files.get(key), Buffer.from(new Uint8Array(os.memory.buffer, addr, size)));
      }
      return decode(c, id, slot, seq, addr, size, flags, end);
    };
    const caps = await probeCaps(VideoDecoder);
    core.publish(caps.map((c) => Object.assign(c, { flags: 1 })), gpu);
    console.error("vdec-stub: vdec ring at " + m.vdec + ", codecs " + caps.map((c) => c.codec).join(",")
                  + (gpu ? ", frames drawn (marked)" : ", copies only"));
    clearInterval(ask);
    run(core);
  };

  return {
    done() {
      clearInterval(ask);
      channel.close();
      for (const fd of files.values()) {
        fs.closeSync(fd);
      }
      if (core) {
        console.error("vdec-stub: " + JSON.stringify(core.stats));
      }
      return chained && chained.done ? chained.done() : undefined;
    },
  };
};
