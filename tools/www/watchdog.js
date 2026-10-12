// Watches the page's main thread from a thread of its own (dev.html starts
// it). The machine cannot do without the main thread: tasks are Workers, and
// only it can make one; the terminal and the network go through it. It
// counts up a shared word whenever this thread asks it to (a message every
// second), and says in another which call into the browser it is in when
// that call may take long. If the count
// stands still for five seconds, this reports where the main thread is,
// and again every fifteen seconds while it lasts: to the server's log (a request for hang-report, which does not
// exist), and to the console.

"use strict";

const NAMES = ["its own code", "drawing the screen (putImageData)", "posting a message to a worker", "sending on the network",
               "making a worker", "writing to the terminal", "a screenshot (toDataURL)", "ending a worker (terminate)",
               "waking a worker (Atomics.notify)"];
const METHODS = ["", "start_primary", "start_secondary", "stop_secondary", "create_and_run_task", "release_task",
                 "serialize_tasks", "console_read", "fb_show", "net_tx", "console_write", "log", "ready", "failed"];

onmessage = (event) => {
  // [0] the count, [1] the call, [2] whether a frame of the display is being drawn, [3] the worker's message being answered
  const words = event.data.words;
  let seen = Atomics.load(words, 0), since = Date.now(), reported = false, again = 0;
  setInterval(() => {
    const now = Atomics.load(words, 0);
    postMessage(0);  // "count": answered when the main thread next handles messages
    if (now != seen) {
      if (reported) {
        console.warn("watchdog: the main thread is back after " + Math.round((Date.now() - since) / 1000) + " s");
        fetch("hang-report?back-after=" + Math.round((Date.now() - since) / 1000), { cache: "no-store" }).catch(() => {});
      }
      seen = now;
      since = Date.now();
      reported = false;
    } else if (Date.now() - since > 5000 && Date.now() > again) {
      const where = NAMES[Atomics.load(words, 1)] || "call " + Atomics.load(words, 1);
      reported = true;
      again = Date.now() + 15000;  // (and again while it lasts: a server's log scrolls)
      console.warn("watchdog: the main thread has not run for 5 s; it is in " + where
                   + (Atomics.load(words, 2) ? ", drawing a frame" : ""));
      const answering = METHODS[Atomics.load(words, 3)] || (Atomics.load(words, 3) ? "another message" : "none");
      console.warn("watchdog: ... answering the message: " + answering);
      fetch("hang-report?in=" + encodeURIComponent(where) + "&answering=" + answering + "&frame=" + Atomics.load(words, 2)
            + "&for=" + Math.round((Date.now() - since) / 1000), { cache: "no-store" })
        .catch(() => {});
    }
  }, 1000);
};
