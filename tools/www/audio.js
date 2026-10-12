// The machine's sound in the page: the main thread's side of the player (audio-worklet.js does the playing).
//
// The kernel's sound card (sound/drivers/wasm_audio.c, patches/kernel-audio.diff) keeps its substreams' samples in
// the machine's memory and a header that says where; the runtime tells the page where the header is
// (patch-runtime.py 25: os.audio). This makes the page's one AudioContext, loads audio-worklet.js into it and
// gives the worklet the machine's memory, the header and the word in which the host raises the kernel's
// interrupts. The worklet reads the samples, resamples them to the context's rate, mixes the substreams and
// counts what it took: that count is the card's hardware pointer.
//
// It also tells the kernel, in the header, how things stand with the player (host_state): nobody plays yet (the
// browser lets a page make sound only once the person has clicked or typed on it: until then the kernel runs the
// streams by its own clock, the sound is lost and programs do not hang), the player is starting (the context is
// being resumed: the kernel waits up to a second for it), or it plays. And how long from the ring to the speaker
// (latency_us: the context's baseLatency and outputLatency), which programs that keep picture and sound together
// are told (ALSA's delay).
//
// The context runs while something wants it: a substream of the card is open, or was in the last 30 s, or
// another part of the page holds it (background.js's tone while the page is hidden: rvAudio.hold()/release()).
// Otherwise it is suspended (the computer's audio output is let go). A page that has been clicked on once may
// resume its context at any time (sticky activation), so a program that starts playing later is heard: the
// open device is seen within 100 ms (1 s while the page is hidden, where timers are slowed; background.js keeps
// the context running then anyway).
//
// ?audio=0: no player at all (the kernel's clock carries the streams; background.js makes its own context).
// window.rvAudio.state() says what is going on: the context, the header, each substream.
(() => {
  "use strict";

  const params = new URLSearchParams(location.search);
  const off = params.get("audio") === "0" || params.get("audio") === "off";
  const version = Date.now();
  // The header's words (struct wasm_audio_header).
  const H_MAGIC = 0, H_STREAMS = 8, H_STREAM_SIZE = 12, H_STREAM_OFFSET = 16, H_VOLUME = 20, H_UNMUTED = 36,
        H_HOST_STATE = 48, H_HEARTBEAT = 52, H_LATENCY_US = 56, H_HOST_RATE = 60;
  const MAGIC = 0x55415652, NO_PLAYER = 0, PLAYING = 1, STARTING = 2;
  const KEEP_MS = 30000;

  let context = null, node = null, nodeFailed = "", header = 0, sentIrq = 0, sentBuffer = null;
  let lastOpen = 0, timer = 0;
  const holds = new Set();
  const notes = [];
  const note = (text) => {
    notes.push(text);
    if (notes.length > 20) notes.shift();
  };

  const os = () => window.linuxOs || null;
  const words = () => {
    const machine = os();
    if (!machine || !header) return null;
    const buffer = machine.memory.buffer;
    return header + 1024 <= buffer.byteLength ? new Int32Array(buffer, header, 1024 / 4) : null;
  };

  // The page's AudioContext, made now if there is none (from a click or a key, or it starts suspended).
  const make = () => {
    if (!context) {
      try {
        context = new AudioContext({ latencyHint: "playback" });
        context.onstatechange = () => update();
        lastOpen = performance.now();	// (it was made running: let it run a while)
        note("context made at " + context.sampleRate + " Hz, " + context.state);
      } catch (error) {
        note("no AudioContext: " + (error && error.message || error));
        context = null;
      }
    }
    if (context && !off && !node && !nodeFailed) {
      nodeFailed = "loading";
      context.audioWorklet.addModule("audio-worklet.js?v=" + version).then(() => {
        node = new AudioWorkletNode(context, "rv-audio", { numberOfInputs: 0, numberOfOutputs: 1,
                                                             outputChannelCount: [2] });
        node.connect(context.destination);
        nodeFailed = "";
        note("player ready");
        update();
      }, (error) => {
        nodeFailed = "audio-worklet.js: " + (error && error.message || error);
        note(nodeFailed);
      });
    }
    return context;
  };

  // Give the worklet the memory and the header, and the interrupt word once it is known.
  const connect = () => {
    const machine = os();
    if (!node || !machine || !machine.audio) return;
    if (!header) {
      header = machine.audio.header();
      if (!header) return;
      note("sound card header at 0x" + header.toString(16));
    }
    const irq = machine.audio.irq_word();
    if (sentBuffer === null) {
      // The memory itself (a shared one may be posted), so that the worklet sees it grow; failing that, its buffer.
      try {
        node.port.postMessage({ memory: machine.memory, header: header, irqWord: irq });
        sentBuffer = false;
      } catch (error) {
        node.port.postMessage({ memory: machine.memory.buffer, header: header, irqWord: irq });
        sentBuffer = machine.memory.buffer;
        note("memory not postable (" + (error && error.message || error) + "): its buffer instead");
      }
      sentIrq = irq;
    } else if (sentBuffer && sentBuffer !== machine.memory.buffer) {
      // (The memory grew: the newer buffer.)
      sentBuffer = machine.memory.buffer;
      node.port.postMessage({ memory: sentBuffer, header: header, irqWord: irq });
      sentIrq = irq;
    } else if (irq && irq !== sentIrq) {
      node.port.postMessage({ irqWord: irq });
      sentIrq = irq;
    }
  };

  // Whether a substream of the card is open now.
  const anyOpen = (w) => {
    const count = w[H_STREAMS / 4], size = w[H_STREAM_SIZE / 4], offset = w[H_STREAM_OFFSET / 4];
    for (let i = 0; i < count; i++) {
      if (Atomics.load(w, (offset + i * size + 60) / 4)) return true;
    }
    return false;
  };

  const update = () => {
    connect();
    const w = words();
    const card = w && Atomics.load(w, H_MAGIC / 4) === MAGIC;
    const now = performance.now();
    if (card && anyOpen(w)) {
      lastOpen = now;
    }
    if (context) {
      const want = holds.size > 0 || now - lastOpen < KEEP_MS;
      if (want && context.state === "suspended") {
        context.resume().catch((error) => note("resume: " + (error && error.message || error)));
      } else if (!want && context.state === "running") {
        context.suspend().catch(() => {});
      }
    }
    if (card) {
      const state = !node ? (context && nodeFailed === "loading" ? STARTING : NO_PLAYER)
                    : context.state === "running" ? PLAYING
                    : context.state === "closed" ? NO_PLAYER : STARTING;
      Atomics.store(w, H_HOST_STATE / 4, state);
      if (context) {
        Atomics.store(w, H_HOST_RATE / 4, context.sampleRate);
        const latency = (context.baseLatency || 0) + (context.outputLatency || 0);
        Atomics.store(w, H_LATENCY_US / 4, Math.round(latency * 1e6));
      }
    }
  };

  if (!off) {
    // The first click or key: the context (and with it the player) may start.
    const first = () => {
      window.removeEventListener("pointerdown", first, true);
      window.removeEventListener("keydown", first, true);
      make();
      update();
    };
    window.addEventListener("pointerdown", first, true);
    window.addEventListener("keydown", first, true);
    timer = setInterval(update, 100);
  }

  window.rvAudio = {
    off: off,
    // The page's AudioContext for other parts of the page (background.js), made if there is none yet; null with
    // ?audio=0 (they make their own).
    sharedContext: () => off ? null : make(),
    // Keep the context running while `name` holds it (background.js while the page is hidden).
    hold(name) {
      holds.add(name);
      update();
    },
    release(name) {
      holds.delete(name);
      update();
    },
    state() {
      const w = words();
      const result = { off, context: context ? context.state : "none", rate: context ? context.sampleRate : 0,
                       player: node ? "ready" : nodeFailed || "not yet (it starts at the first click or key)",
                       holds: [...holds], header: header ? "0x" + header.toString(16) : "not known", notes };
      if (w && Atomics.load(w, H_MAGIC / 4) === MAGIC) {
        const h64 = new BigUint64Array(w.buffer, header, 128);
        result.hostState = ["no player", "playing", "starting"][w[H_HOST_STATE / 4]];
        result.heartbeat = w[H_HEARTBEAT / 4];
        result.latencyMs = w[H_LATENCY_US / 4] / 1000;
        result.volume = w[H_VOLUME / 4] + (w[H_UNMUTED / 4] ? "" : " (muted)");
        result.substreams = [];
        const count = w[H_STREAMS / 4], size = w[H_STREAM_SIZE / 4], offset = w[H_STREAM_OFFSET / 4];
        for (let i = 0; i < count; i++) {
          const at = offset + i * size;
          result.substreams.push({
            open: w[(at + 60) / 4], state: ["stopped", "running", "paused"][w[(at + 4) / 4]],
            rate: w[(at + 24) / 4], channels: w[(at + 28) / 4], format: w[(at + 32) / 4],
            position: Number(BigInt.asUintN(64, h64[(at + 64) / 8] - h64[(at + 40) / 8])),
            underruns: w[(at + 72) / 4], interrupts: w[(at + 76) / 4],
          });
        }
      }
      return result;
    },
  };
  void timer;
})();
