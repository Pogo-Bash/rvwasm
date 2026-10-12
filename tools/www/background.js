// The machine at full speed while the page is not in front.
//
// What Chrome does to a page it does not show (another tab or another app in front, the window minimized or
// on another Space, or covered), as its source has it (2026-10):
//
//   - requestAnimationFrame stops altogether. The page draws nothing, and the window server's clients waited
//     for frame callbacks that only a drawn frame gave: display.js now tells the server, which stands in for
//     the page at a low rate (wlserver/compositor.js, the table's page_hidden word).
//   - The main thread's timers run at most once a second, and after five minutes hidden, for chains of
//     timers, once a minute ("intensive wake-up throttling"), unless the page has made sound in the last 30
//     seconds or uses WebRTC (developer.chrome.com/blog/timer-throttling-in-chrome-88). Nothing the machine
//     needs runs on a timer of the main thread: the kernel's clock is a wait with a timeout in the Workers
//     (arch/wasm/kernel/smp.c), and Workers' timers are not throttled (Blink's kDedicatedWorkerThrottling,
//     "BlinkSchedulerWorkerThrottling", is off by default). Messages from the Workers (a new task, the
//     console, the network) are not timers either. The console's input no longer waits for the main thread
//     at all (patch-runtime.py, 17).
//   - The whole renderer process, every Worker of the machine with it, is given a low priority: Chrome's
//     process priority policy (performance_manager, kPMProcessPriorityPolicy, on by default) votes a frame
//     up only while it is visible, audible, or capturing media (frame_visibility_voter.cc,
//     frame_audible_voter.cc, frame_capturing_media_stream_voter.cc); otherwise the process is "best
//     effort", which on a Mac is TASK_BACKGROUND_APPLICATION with the task suppression policy: low CPU
//     priority, CPU use suppressed, and its timers coalesced at LATENCY_QOS_TIER_5 (base/process/
//     process_mac.cc). So the machine slows down, and the kernel's timers fire late (`timeout 60` took longer
//     than 60 s). Workers cannot get out of that; only the page can.
//   - After five minutes hidden and silent, with Energy Saver on, a page that uses much CPU is frozen,
//     Workers and all (developer.chrome.com/blog/freezing-on-energy-saver); Memory Saver may discard a page
//     that is not audible.
//
// What exempts a page from all of these is sound: an audible page. "Audible" is measured on what the page
// plays: at least -72.25 dBFS of average power (services/audio/output_stream.cc, kSilenceThresholdDBFS), so a
// silent AudioContext does not count. So while the page is hidden this plays a sine of 1 Hz at an amplitude of
// 0.003 (about -53 dBFS): far below anything a speaker or an ear makes anything of at that frequency, and
// well above the threshold. When the page is shown again it stops (it fades out over 50 ms, then the context
// is suspended). The price: while the page is hidden Chrome shows the speaker on its tab and, as for any page
// playing sound, keeps the computer from going to sleep by itself.
//
// A page may start sound only after the person has clicked or typed on it (Chrome's autoplay policy), so the
// AudioContext is made at the first click or key; a page hidden before that runs slowly as before.
//
// The context is the page's one AudioContext, which the machine's sound plays in as well (audio.js, loaded
// before this): the tone holds it running while the page is hidden (rvAudio.hold) and lets go of it when the page
// is shown, instead of suspending it, which would silence the machine. With ?audio=0 there is no such context
// and this makes its own.
//
// ?background=off (or rvBackground.set("off"), kept in this browser) turns it off; ?background=audio, or
// rvBackground.set("audio"), on again. window.rvBackground.state() says what it is doing.
(() => {
  "use strict";

  const AMPLITUDE = 0.003, FREQUENCY = 1, FADE = 0.05;
  const asked = new URLSearchParams(location.search).get("background");
  let saved = null;
  try {
    saved = localStorage.getItem("rv-background");
  } catch (error) {
    // (no storage here)
  }
  let mode = asked === "off" || asked === "audio" ? asked : saved === "off" ? "off" : "audio";

  const state = { mode, audio: "not yet (it starts at the first click or key on the page)", hiddenCount: 0,
                  playedMs: 0, playingSince: 0, error: "" };
  let context = null, gain = null, stopTimer = 0, shared = false;
  const page = () => window.rvAudio && window.rvAudio.sharedContext ? window.rvAudio : null;

  const make = () => {
    if (context || mode !== "audio") {
      return;
    }
    try {
      context = page() && page().sharedContext();
      shared = !!context;
      if (!context) {
        context = new AudioContext({ latencyHint: "playback" });
      }
      const oscillator = new OscillatorNode(context, { type: "sine", frequency: FREQUENCY });
      gain = new GainNode(context, { gain: 0 });
      oscillator.connect(gain).connect(context.destination);
      oscillator.start();
      context.addEventListener("statechange", () => {
        state.audio = context.state;
      });
      state.audio = context.state;
      if (document.hidden) {
        play();
      } else if (shared) {
        page().release("background");
      } else {
        context.suspend().catch(() => {});
      }
    } catch (error) {
      state.error = String(error && error.message || error);
      state.audio = "failed";
      context = null;
    }
  };

  const play = () => {
    if (!context || mode !== "audio") {
      return;
    }
    clearTimeout(stopTimer);
    stopTimer = 0;
    if (shared) {
      page().hold("background");
    }
    context.resume().then(() => {
      const now = context.currentTime;
      gain.gain.cancelScheduledValues(now);
      gain.gain.setValueAtTime(gain.gain.value, now);
      gain.gain.linearRampToValueAtTime(AMPLITUDE, now + FADE);
      if (!state.playingSince) {
        state.playingSince = performance.now();
      }
    }, (error) => {
      state.error = "resume: " + String(error && error.message || error);
    });
  };

  const stop = () => {
    if (state.playingSince) {
      state.playedMs += performance.now() - state.playingSince;
      state.playingSince = 0;
    }
    if (!context || context.state === "closed") {
      return;
    }
    const now = context.currentTime;
    gain.gain.cancelScheduledValues(now);
    gain.gain.setValueAtTime(gain.gain.value, now);
    gain.gain.linearRampToValueAtTime(0, now + FADE);
    clearTimeout(stopTimer);
    stopTimer = setTimeout(() => {
      stopTimer = 0;
      if (context && (document.hidden === false || mode !== "audio")) {
        if (shared) {
          page().release("background");
        } else {
          context.suspend().catch(() => {});
        }
      }
    }, FADE * 1000 * 2);
  };

  const first = () => {
    window.removeEventListener("pointerdown", first, true);
    window.removeEventListener("keydown", first, true);
    make();
  };
  window.addEventListener("pointerdown", first, true);
  window.addEventListener("keydown", first, true);

  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      state.hiddenCount++;
      play();
    } else {
      stop();
    }
  });

  window.rvBackground = {
    // "audio" (the default) or "off"; kept in this browser for the next time the page is opened.
    set(value) {
      mode = value === "off" ? "off" : "audio";
      state.mode = mode;
      try {
        localStorage.setItem("rv-background", mode);
      } catch (error) {
        // (for this page only)
      }
      if (mode === "off") {
        stop();
      } else if (!context) {
        make();	// (if this is called from a click or a key; otherwise at the next one)
      } else if (document.hidden) {
        play();
      }
      return mode;
    },
    state: () => Object.assign({}, state, {
      contextState: context ? context.state : "none",
      sharedContext: shared,
      playedMs: state.playedMs + (state.playingSince ? performance.now() - state.playingSince : 0),
    }),
  };
})();
