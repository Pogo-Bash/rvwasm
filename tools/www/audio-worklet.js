// The machine's sound, played: the AudioWorklet side of tools/www/audio.js.
//
// The kernel's sound card (sound/drivers/wasm_audio.c in patches/kernel-audio.diff) keeps a header in the
// machine's memory: a block for each of its playback substreams, which says where the substream's ring is (its
// DMA buffer: the samples the program wrote), in what format, at what rate, and whether it runs. This reads the
// samples straight out of the shared memory, converts them to floats, resamples them to the AudioContext's rate,
// mixes the substreams and applies the master volume. What it has taken out of a ring it counts in the block
// (consumed): that count is the card's hardware pointer, so the programs' timing follows what is played here.
// When a substream crosses a period boundary this raises the kernel's WASM_IRQ_AUDIO (on the word the runtime
// says, as the main thread raises the input interrupt), and ALSA hears of it at once.
//
// Past what the program has written (valid_end: an underrun, the end of a drain) it plays silence, though it
// counts on: the hardware pointer of a running substream runs on, as a sound card's does. For every block it
// renders it ticks the header's heartbeat: while that moves the kernel leaves the pointer to this.
//
// The header's layout is the kernel's (struct wasm_audio_header, struct wasm_audio_stream), offsets in bytes.
//
// The mixing is a class of its own (RvAudioMixer) so that tools/audio-consumer.js can run the same code under
// Node, against tools/node-machine.js, without a browser.
(function () {
  "use strict";

  const MAGIC = 0x55415652;	// "RVAU"
  // The header (struct wasm_audio_header).
  const H_MAGIC = 0, H_STREAMS = 8, H_STREAM_SIZE = 12, H_STREAM_OFFSET = 16, H_VOLUME = 20, H_DB_MIN = 28,
        H_DB_STEP = 32, H_UNMUTED = 36, H_IRQ = 40, H_HEARTBEAT = 52, H_SIZE = 64;
  // A substream's block (struct wasm_audio_stream).
  const S_GENERATION = 0, S_STATE = 4, S_BUFFER = 8, S_BUFFER_FRAMES = 16, S_PERIOD_FRAMES = 20, S_RATE = 24,
        S_CHANNELS = 28, S_FORMAT = 32, S_BASE = 40, S_VALID_END = 48, S_DIRECTION = 56, S_CONSUMED = 64,
        S_UNDERRUNS = 72, S_INTERRUPTS = 76;
  const RUNNING = 1;
  // SNDRV_PCM_FORMAT_*
  const FORMAT_U8 = 1, FORMAT_S16_LE = 2, FORMAT_S24_LE = 6, FORMAT_S32_LE = 10, FORMAT_FLOAT_LE = 14;
  // Input frames kept for resampling, per channel: a block of 128 at up to 24 times the output rate, and more.
  const FIFO = 8192;

  class RvAudioMixer {
    // memory: the machine's WebAssembly.Memory (or its SharedArrayBuffer); header: the header's address;
    // irqWord: the word the host raises the kernel's interrupts in (0 if not known); rate: the output's rate.
    constructor(memory, header, irqWord, rate) {
      this.memory = memory;
      this.header = header;
      this.irqWord = irqWord;
      this.rate = rate;
      this.buffer = null;
      this.gain = 0;
      this.streams = [];
    }

    // Views of the memory as it is now (a memory that has grown has a new buffer; the old one stays valid
    // for what it covered).
    refresh() {
      const buffer = this.memory instanceof SharedArrayBuffer ? this.memory : this.memory.buffer;
      if (buffer !== this.buffer) {
        this.buffer = buffer;
        this.h32 = new Int32Array(buffer, this.header, 1024 / 4);
        this.h64 = new BigUint64Array(buffer, this.header, 1024 / 8);
        this.irq64 = this.irqWord ? new BigInt64Array(buffer, this.irqWord, 1) : null;
        this.irq32 = this.irqWord ? new Int32Array(buffer, this.irqWord, 1) : null;
        for (const s of this.streams) {
          s.generation = -1;	// (its view of the ring is made again)
        }
      }
    }

    setIrqWord(at) {
      this.irqWord = at;
      this.buffer = null;
    }

    // Raise the kernel's interrupt `irq` on IRQ_CPU: its bit in the word, and a notify (as raise_interrupt()).
    raise(irq) {
      if (this.irq64) {
        Atomics.or(this.irq64, 0, 1n << BigInt(irq));
        Atomics.notify(this.irq32, 0, 1);
      }
    }

    // Render `frames` frames into the arrays `left` and `right` (cleared first). Returns whether a substream ran.
    render(left, right, frames) {
      left.fill(0);
      right.fill(0);
      if (!this.header) {
        return false;
      }
      this.refresh();
      const h32 = this.h32;
      if (Atomics.load(h32, H_MAGIC / 4) !== MAGIC) {
        return false;
      }
      Atomics.add(h32, H_HEARTBEAT / 4, 1);
      const count = h32[H_STREAMS / 4], size = h32[H_STREAM_SIZE / 4], offset = h32[H_STREAM_OFFSET / 4];
      let ran = false;
      for (let i = 0; i < count; i++) {
        const at = offset + i * size;
        if (!this.streams[i]) {
          this.streams[i] = { at: at, generation: -1, fifo: [new Float32Array(FIFO), new Float32Array(FIFO)],
                              length: 1, phase: 0, running: false };
        }
        const s = this.streams[i];
        if (h32[(at + S_DIRECTION) / 4] !== 0 || Atomics.load(h32, (at + S_STATE) / 4) !== RUNNING) {
          if (s.running) {
            this.reset(s);
          }
          continue;
        }
        if (this.stream(s, left, right, frames)) {
          ran = true;
        }
      }

      // The master volume and switch, eased over the block.
      const volume = h32[H_VOLUME / 4];
      const target = !h32[H_UNMUTED / 4] || volume <= 0 ? 0
                     : Math.pow(10, (h32[H_DB_MIN / 4] + volume * h32[H_DB_STEP / 4]) / 2000);
      const from = this.gain, step = (target - from) / frames;
      if (from !== 1 || target !== 1) {
        for (let k = 0; k < frames; k++) {
          const g = from + step * (k + 1);
          left[k] *= g;
          right[k] *= g;
        }
      }
      this.gain = target;
      return ran;
    }

    reset(s) {
      s.running = false;
      s.length = 1;	// (the sample before the first: silence)
      s.fifo[0][0] = 0;
      s.fifo[1][0] = 0;
      s.phase = 0;
    }

    // Take on a substream's parameters (at a new generation: the kernel prepared it again).
    setup(s) {
      const h32 = this.h32, at = s.at;
      s.generation = Atomics.load(h32, (at + S_GENERATION) / 4);
      const address = Number(this.h64[(at + S_BUFFER) / 8]);
      s.frames = h32[(at + S_BUFFER_FRAMES) / 4];
      s.period = h32[(at + S_PERIOD_FRAMES) / 4];
      s.channels = h32[(at + S_CHANNELS) / 4];
      s.format = h32[(at + S_FORMAT) / 4];
      s.step = h32[(at + S_RATE) / 4] / this.rate;
      const samples = s.frames * s.channels;
      const types = { [FORMAT_U8]: Uint8Array, [FORMAT_S16_LE]: Int16Array, [FORMAT_S24_LE]: Int32Array,
                      [FORMAT_S32_LE]: Int32Array, [FORMAT_FLOAT_LE]: Float32Array };
      const Type = types[s.format];
      s.ring = null;
      if (Type && s.frames > 0 && s.period > 0 && s.channels >= 1 && s.channels <= 2 && s.step > 0 &&
          address % Type.BYTES_PER_ELEMENT === 0) {
        // (A memory that has grown since this buffer was taken: its newer buffer covers the ring.)
        if (address + samples * Type.BYTES_PER_ELEMENT > this.buffer.byteLength && !(this.memory instanceof SharedArrayBuffer)) {
          this.refresh();
        }
        if (address + samples * Type.BYTES_PER_ELEMENT <= this.buffer.byteLength) {
          s.ring = new Type(this.buffer, address, samples);
        }
      }
      this.reset(s);
    }

    // One running substream, mixed into left and right.
    stream(s, left, right, frames) {
      const h32 = this.h32, h64 = this.h64, at = s.at;
      if (Atomics.load(h32, (at + S_GENERATION) / 4) !== s.generation) {
        this.setup(s);
      }
      if (!(s.frames > 0 && s.period > 0 && s.step > 0 && s.channels >= 1 && s.channels <= 2)) {
        return false;
      }
      // (A ring this cannot read, a format it does not know: silence, but the substream's time goes on.)
      s.running = true;
      const consumed = Atomics.load(h64, (at + S_CONSUMED) / 8);
      const base = Atomics.load(h64, (at + S_BASE) / 8);
      const validEnd = Number(Atomics.load(h64, (at + S_VALID_END) / 8));
      const start = Number(BigInt.asUintN(64, consumed - base));
      // Fill the FIFO with as many input frames as this block needs (Catmull-Rom: the sample before and two after).
      const need = Math.floor(s.phase + (frames - 1) * s.step) + 4;
      const ring = s.ring, channels = s.channels, format = s.format, size = s.frames;
      const f0 = s.fifo[0], f1 = s.fifo[1];
      let pos = start, missing = 0;
      while (s.length < need && s.length < FIFO) {
        let a = 0, b = 0;
        if (pos < validEnd && ring) {
          const i = (pos % size) * channels;
          a = ring[i];
          b = channels === 2 ? ring[i + 1] : a;
          if (format === FORMAT_S16_LE) {
            a /= 32768;
            b /= 32768;
          } else if (format === FORMAT_S32_LE) {
            a /= 2147483648;
            b /= 2147483648;
          } else if (format === FORMAT_S24_LE) {
            a = ((a << 8) >> 8) / 8388608;
            b = ((b << 8) >> 8) / 8388608;
          } else if (format === FORMAT_U8) {
            a = (a - 128) / 128;
            b = (b - 128) / 128;
          }
        } else {
          missing++;
        }
        f0[s.length] = a;
        f1[s.length] = b;
        s.length++;
        pos++;
      }
      if (pos !== start) {
        Atomics.store(h64, (at + S_CONSUMED) / 8, consumed + BigInt(pos - start));
        if (missing) {
          Atomics.add(h32, (at + S_UNDERRUNS) / 4, 1);
        }
        if (Math.floor(pos / s.period) !== Math.floor(start / s.period)) {
          Atomics.add(h32, (at + S_INTERRUPTS) / 4, 1);
          this.raise(h32[H_IRQ / 4]);
        }
      }

      // Resample: output frame k is at phase + k * step past FIFO index 1.
      let phase = s.phase;
      for (let k = 0; k < frames; k++, phase += s.step) {
        const whole = Math.floor(phase), t = phase - whole, j = 1 + whole;
        if (j + 2 >= s.length) {
          break;	// (only if the FIFO is full: a rate beyond what it is made for)
        }
        let p = f0[j - 1], q = f0[j], r = f0[j + 1], u = f0[j + 2];
        left[k] += q + 0.5 * t * (r - p + t * (2 * p - 5 * q + 4 * r - u + t * (3 * (q - r) + u - p)));
        p = f1[j - 1]; q = f1[j]; r = f1[j + 1]; u = f1[j + 2];
        right[k] += q + 0.5 * t * (r - p + t * (2 * p - 5 * q + 4 * r - u + t * (3 * (q - r) + u - p)));
      }
      const total = s.phase + frames * s.step, used = Math.min(Math.floor(total), s.length - 1);
      s.phase = total - Math.floor(total);
      f0.copyWithin(0, used, s.length);
      f1.copyWithin(0, used, s.length);
      s.length -= used;
      return true;
    }
  }

  if (typeof registerProcessor === "function") {
    // In the page: the processor. The main thread (audio.js) sends {memory, header, irqWord} once it knows them,
    // and {irqWord} again if that comes later.
    class RvAudioProcessor extends AudioWorkletProcessor {
      constructor() {
        super();
        this.mixer = null;
        this.port.onmessage = (event) => {
          const m = event.data || {};
          if (m.memory && m.header) {
            this.mixer = new RvAudioMixer(m.memory, m.header, m.irqWord || 0, sampleRate);
          } else if (m.irqWord && this.mixer) {
            this.mixer.setIrqWord(m.irqWord);
          }
        };
      }

      process(inputs, outputs) {
        const out = outputs[0];
        if (this.mixer && out && out.length >= 2) {
          this.mixer.render(out[0], out[1], out[0].length);
        } else if (this.mixer && out && out.length === 1) {
          this.mixer.render(out[0], new Float32Array(out[0].length), out[0].length);
        }
        return true;
      }
    }
    registerProcessor("rv-audio", RvAudioProcessor);
  }
  if (typeof module === "object" && module.exports) {
    module.exports = { RvAudioMixer };
  }
})();
