// The page's player without a browser: a --hook for tools/node-machine.js that plays the machine's sound card
// (sound/drivers/wasm_audio.c) as tools/www/audio.js and audio-worklet.js do, in real time, with the same mixing
// code (RvAudioMixer), into a WAV file instead of a speaker. For trying the kernel's driver and what runs on it
// natively (tests/audio/run.sh).
//
//   node --experimental-wasm-memory64 tools/node-machine.js ... --hook tools/audio-consumer.js 'aplay ...'
//
// Environment:
//   AUDIO_OUT=FILE     where the WAV goes (default build/audio-out.wav); its statistics go to standard error
//   AUDIO_RATE=HZ      the output's rate (48000)
//   AUDIO_HOST=none    play nothing and say there is no player (the kernel's clock must then carry the streams)
//   AUDIO_STOP_MS=N    stop playing N ms after the command started, as a page whose output stalls
//   AUDIO_LATENCY_US   the output latency to report (20000)
"use strict";
const fs = require("fs");
const path = require("path");
const { RvAudioMixer } = require(path.join(__dirname, "www", "audio-worklet.js"));

const H_HOST_STATE = 48, H_HEARTBEAT = 52, H_LATENCY_US = 56, H_HOST_RATE = 60;

module.exports = (os, info) => {
  const rate = Number(process.env.AUDIO_RATE) || 48000;
  const out = process.env.AUDIO_OUT || path.join(__dirname, "..", "build", "audio-out.wav");
  const none = process.env.AUDIO_HOST === "none";
  const stopAt = Number(process.env.AUDIO_STOP_MS) || 0;
  const began = performance.now();
  const max = rate * 120;
  const left = new Float32Array(max), right = new Float32Array(max);
  const block = { l: new Float32Array(128), r: new Float32Array(128) };
  let rendered = 0, mixer = null, header = 0, timer = null, stopped = false, played = 0;

  const words = () => new Int32Array(os.memory.buffer, header, 16);
  const tick = () => {
    if (!header) {
      header = os.audio ? os.audio.header() : 0;
      if (!header) {
        return;
      }
      const w = words();
      Atomics.store(w, H_HOST_STATE / 4, none ? 0 : 1);
      Atomics.store(w, H_HOST_RATE / 4, rate);
      Atomics.store(w, H_LATENCY_US / 4, Number(process.env.AUDIO_LATENCY_US) || 20000);
      mixer = new RvAudioMixer(os.memory, header, os.audio.irq_word(), rate);
      process.stderr.write("[audio] header at 0x" + header.toString(16) + ", irq word 0x"
                           + os.audio.irq_word().toString(16) + (none ? ", not playing" : "") + "\n");
    }
    if (none || stopped) {
      return;
    }
    if (stopAt && performance.now() - began > stopAt) {
      stopped = true;
      process.stderr.write("[audio] the player stops at " + Math.round(performance.now() - began) + " ms\n");
      return;
    }
    if (!mixer.irqWord && os.audio.irq_word()) {
      mixer.setIrqWord(os.audio.irq_word());
    }
    const due = Math.floor((performance.now() - began) / 1000 * rate);
    while (played + 128 <= due) {
      const ran = mixer.render(block.l, block.r, 128);
      played += 128;
      if ((ran || rendered) && rendered + 128 <= max) {
        left.set(block.l, rendered);
        right.set(block.r, rendered);
        rendered += 128;
      }
    }
  };
  timer = setInterval(tick, 2);

  return {
    done: () => {
      clearInterval(timer);
      // Statistics: per substream what was taken, underruns, interrupts; of the sound, its level and pitch.
      const lines = [];
      if (header) {
        const buffer = os.memory.buffer;
        const h32 = new Int32Array(buffer, header, 256), h64 = new BigUint64Array(buffer, header, 128);
        lines.push("heartbeat " + h32[H_HEARTBEAT / 4] + ", volume " + h32[5] + "/" + h32[6] + ", unmuted " + h32[9]);
        for (let i = 0; i < h32[2]; i++) {
          const at = (64 + i * 128);
          const g = h32[at / 4], consumed = h64[(at + 64) / 8], base = h64[(at + 40) / 8];
          if (!g) continue;
          lines.push("substream " + i + ": generation " + g + ", state " + h32[at / 4 + 1] + ", rate " + h32[(at + 24) / 4]
                     + ", channels " + h32[(at + 28) / 4] + ", format " + h32[(at + 32) / 4]
                     + ", position " + BigInt.asUintN(64, consumed - base) + ", consumed " + consumed
                     + ", underruns " + h32[(at + 72) / 4] + ", interrupts " + h32[(at + 76) / 4]);
        }
      } else {
        lines.push("no sound card (os.audio.header() stayed 0)");
      }
      // The loudest second, its RMS and its frequency by rising zero crossings.
      let best = 0, bestRms = 0;
      for (let s = 0; s + rate <= rendered; s += rate / 4) {
        let e = 0;
        for (let k = s; k < s + rate; k += 4) e += left[k] * left[k];
        if (e > bestRms) { bestRms = e; best = s; }
      }
      let n = 0, first = -1, last = -1, sum = 0;
      for (let k = best + 1; k < Math.min(rendered, best + rate); k++) {
        sum += left[k] * left[k];
        if (left[k - 1] < 0 && left[k] >= 0) { if (first < 0) first = k; else n++; last = k; }
      }
      const seconds = rendered / rate;
      lines.push("rendered " + seconds.toFixed(2) + " s; loudest second at " + (best / rate).toFixed(2) + " s: rms "
                 + Math.sqrt(sum / rate).toFixed(4) + ", " + (n ? (n * rate / (last - first)).toFixed(1) : "0") + " Hz");
      process.stderr.write(lines.map((l) => "[audio] " + l + "\n").join(""));
      // The WAV: 16-bit stereo.
      const data = Buffer.alloc(rendered * 4);
      for (let k = 0; k < rendered; k++) {
        data.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(left[k] * 32767))), 4 * k);
        data.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(right[k] * 32767))), 4 * k + 2);
      }
      const head = Buffer.alloc(44);
      head.write("RIFF", 0); head.writeUInt32LE(36 + data.length, 4); head.write("WAVE", 8); head.write("fmt ", 12);
      head.writeUInt32LE(16, 16); head.writeUInt16LE(1, 20); head.writeUInt16LE(2, 22); head.writeUInt32LE(rate, 24);
      head.writeUInt32LE(rate * 4, 28); head.writeUInt16LE(4, 32); head.writeUInt16LE(16, 34); head.write("data", 36);
      head.writeUInt32LE(data.length, 40);
      fs.mkdirSync(path.dirname(out), { recursive: true });
      fs.writeFileSync(out, Buffer.concat([head, data]));
      process.stderr.write("[audio] wrote " + out + "\n");
    },
  };
};
