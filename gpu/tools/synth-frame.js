"use strict";
// VK-14: stand-ins for the page's video frames, for tools that have no WebCodecs (Node): the test card the stand-in decoder
// (vdec-stub.js) makes its pictures of, and SynthFrame, an object with the parts of VideoFrame that tools/www/rvgpu-worker.js
// uses, made from the card's descriptor (rvgpu-nm-worker.js in the executor's Worker; vdec-draw-test.js).

// The test card (VK-14), BT.709 full range as the decoder is told to expect: top 45% eight bars (white, yellow, cyan, green,
// magenta, red, blue, black), then a grey ramp to 70%, then black with a white square that moves 8 pixels a picture.
// NV12 as an NV12 VideoFrame's copyTo gives it: Y w x h, then interleaved CbCr with the same stride.
const testCard = (w, h, seq) => {
  const kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
  const bars = [[1, 1, 1], [1, 1, 0], [0, 1, 1], [0, 1, 0], [1, 0, 1], [1, 0, 0], [0, 0, 1], [0, 0, 0]];
  const yuv = (r, g, b) => {
    const y = kr * r + kg * g + kb * b;
    return [y, (b - y) / (2 * (1 - kb)) + 0.5, (r - y) / (2 * (1 - kr)) + 0.5].map((v) => Math.round(Math.min(1, Math.max(0, v)) * 255));
  };
  const Y = new Uint8Array(w * h), UV = new Uint8Array(w * (h >> 1));
  const px = (x, y) => {
    if (y < h * 0.45) return yuv(...bars[Math.min(7, Math.floor(x * 8 / w))]);
    if (y < h * 0.7) return yuv(x / (w - 1), x / (w - 1), x / (w - 1));
    const mx = (seq * 8) % Math.max(1, w - 32);
    return x >= mx && x < mx + 32 && y >= h * 0.8 && y < h * 0.8 + 32 ? yuv(1, 1, 1) : yuv(0, 0, 0);
  };
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const p = px(x, y);
      Y[y * w + x] = p[0];
      if (!(y & 1) && !(x & 1)) {
        UV[(y >> 1) * w + x] = p[1];
        UV[(y >> 1) * w + x + 1] = p[2];
      }
    }
  }
  return { y: Y, uv: UV };
};

// What a browser's VideoFrame gives for the parts rvgpu-worker.js uses, for the stand-in decoder's NV12 frames.
class SynthFrame {
  constructor(d) {
    Object.assign(this, d);
    this.visibleRect = { x: 0, y: 0, width: d.displayWidth, height: d.displayHeight };
    this.closed = false;
  }
  close() {
    this.closed = true;
  }
  clone() {
    return new SynthFrame(this);
  }
  async copyTo(dest, { rect, layout }) {
    if (this.closed) throw new Error("InvalidStateError: closed");
    const w = this.codedWidth;
    for (let r = 0; r < rect.height; r++) {
      dest.set(this.y.subarray((rect.y + r) * w + rect.x, (rect.y + r) * w + rect.x + rect.width), layout[0].offset + r * layout[0].stride);
    }
    for (let r = 0; r < rect.height / 2; r++) {
      const from = ((rect.y >> 1) + r) * w + rect.x;
      dest.set(this.uv.subarray(from, from + rect.width), layout[1].offset + r * layout[1].stride);
    }
    return layout;
  }
  // The frame as RGBA, what a browser draws for it (the frame's matrix and range, nearest chroma; no transfer function).
  rgba() {
    const K = { bt709: [0.2126, 0.0722], bt470bg: [0.299, 0.114], smpte170m: [0.299, 0.114], "bt2020-ncl": [0.2627, 0.0593] };
    const cs = this.colorSpace || {}, full = cs.fullRange !== false;
    const [kr, kb] = K[cs.matrix] || K.bt709, kg = 1 - kr - kb;
    const w = this.displayWidth, h = this.displayHeight, out = new Uint8Array(w * h * 4);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const Y = (this.y[y * this.codedWidth + x] - (full ? 0 : 16)) / (full ? 255 : 219);
        const at = (y >> 1) * this.codedWidth + (x & ~1);
        const cb = (this.uv[at] - 128) / (full ? 255 : 224), cr = (this.uv[at + 1] - 128) / (full ? 255 : 224);
        const r = Y + 2 * (1 - kr) * cr, b = Y + 2 * (1 - kb) * cb, g = (Y - kr * r - kb * b) / kg;
        const o = (y * w + x) * 4;
        out[o] = Math.round(Math.min(1, Math.max(0, r)) * 255);
        out[o + 1] = Math.round(Math.min(1, Math.max(0, g)) * 255);
        out[o + 2] = Math.round(Math.min(1, Math.max(0, b)) * 255);
        out[o + 3] = 255;
      }
    }
    return out;
  }
}

module.exports = { testCard, SynthFrame };
