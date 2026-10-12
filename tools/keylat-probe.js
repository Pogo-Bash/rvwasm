// Key to screen latency, measured in the page (desktop-drive.js eval:tools/keylat-probe.js, then js:keylat.report()).
// Every key the page gets (keydown, in the capture phase, before anything of the page's) and every new frame a
// window of the window server presents (looked at every animation frame: its present count and the damage it
// said) are noted with the page's clock. keylat.report() pairs each key with the frames that came after it and
// before the next key (at most 3 s): the first, the first that changed more than a caret could (an area of
// more than 300 pixels), and the last.
(() => {
  if (window.keylat) {
    return "already there";
  }
  const K = window.keylat = { keys: [], frames: [] };
    // The window server's listener may already be there (it is from the page's start): a key it stops is
  // noted when it stops it.
  const seenKeys = new WeakSet();
  const note = (e) => {
    if (e.type === "keydown" && !seenKeys.has(e)) {
      seenKeys.add(e);
      K.keys.push([performance.now(), e.code]);
    }
  };
  window.addEventListener("keydown", note, true);
  for (const m of ["stopImmediatePropagation", "stopPropagation", "preventDefault"]) {
    const original = Event.prototype[m];
    Event.prototype[m] = function () { note(this); return original.call(this); };
  }
  const seen = new Map();
  const look = () => {
    if (window.rvDisplay && rvDisplay.active()) {
      const now = performance.now();
      for (const w of rvDisplay.windows()) {
        if (seen.get(w.id) !== w.present) {
          if (seen.has(w.id)) {
            const area = w.damage ? w.damage.reduce((a, r) => a + Math.max(0, r[2]) * Math.max(0, r[3]), 0) : w.bw * w.bh;
            K.frames.push([now, w.id, w.kind, area]);
          }
          seen.set(w.id, w.present);
        }
      }
    }
    requestAnimationFrame(look);
  };
  requestAnimationFrame(look);
  K.clear = () => { K.keys.length = 0; K.frames.length = 0; return "cleared"; };
  K.report = (skip = /^(Control|Shift|Alt|Meta)/) => {
    const keys = K.keys.filter((k) => !skip.test(k[1]));
    const rows = [];
    for (let i = 0; i < keys.length; i++) {
      const t = keys[i][0], until = Math.min(i + 1 < keys.length ? keys[i + 1][0] : Infinity, t + 3000);
      const after = K.frames.filter((f) => f[0] > t && f[0] <= until);
      const big = after.find((f) => f[3] > 300);
      rows.push({ key: keys[i][1], first: after.length ? Math.round(after[0][0] - t) : null,
                  changed: big ? Math.round(big[0] - t) : null, last: after.length ? Math.round(after[after.length - 1][0] - t) : null,
                  frames: after.length });
    }
    const med = (a) => { a = a.filter((x) => x !== null).sort((x, y) => x - y); return a.length ? a[a.length >> 1] : null; };
    return JSON.stringify({ keys: rows.length, median_first: med(rows.map((r) => r.first)), median_changed: med(rows.map((r) => r.changed)),
                            median_last: med(rows.map((r) => r.last)), rows });
  };
  return "keylat ready";
})()
