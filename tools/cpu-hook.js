// A hook for tools/node-machine.js: the CPU time of the whole Node process (every worker thread and the engine's
// background threads) and the wall time between markers the command prints: "@@S name" and "@@E name".
// Also looks at the first 64 bytes of the machine's memory (the null page) every 20 ms and says when a word changes.
const marks = {};
module.exports = (os, ctx) => {
  let seen = 0;
  const t0 = process.cpuUsage(), w0 = process.hrtime.bigint();
  const cpu = () => { const u = process.cpuUsage(t0); return (u.user + u.system) / 1e6; };
  const wall = () => Number(process.hrtime.bigint() - w0) / 1e9;
  let last = null;
  // The cores in use per 250 ms window, over the whole run and over each marked part.
  const windows = [];
  let wprev = process.cpuUsage(), wprev_t = process.hrtime.bigint();
  const sampler = setInterval(() => {
    const u = process.cpuUsage(), t = process.hrtime.bigint();
    const dt = Number(t - wprev_t) / 1e9;
    windows.push({ t: wall(), cores: ((u.user - wprev.user) + (u.system - wprev.system)) / 1e6 / dt });
    wprev = u; wprev_t = t;
  }, 250);
  const timer = setInterval(() => {
    const text = ctx.output();
    const re = /@@([SE]) (\S+)/g;
    re.lastIndex = seen;
    let m;
    while ((m = re.exec(text))) {
      marks[m[1] + m[2]] = { cpu: cpu(), wall: wall() };
      if (m[1] == "E" && os.jit && os.jit.stats) console.error("JITSTATS " + m[2] + " " + JSON.stringify(os.jit.stats()) + " programs " + (os.jit.programs ? os.jit.programs().length : "-"));
      seen = re.lastIndex;
    }
    if (process.env.NULLPAGE && os.memory) {
      const now = Array.from(new Uint32Array(os.memory.buffer, 0, 16)).join(",");
      if (last === null) console.error("[nullpage " + wall().toFixed(2) + "s] initial " + now);
      if (last !== null && now !== last) console.error("[nullpage " + wall().toFixed(2) + "s] " + last + " -> " + now);
      last = now;
    }
  }, Number(process.env.NULLPAGE_MS || 20));
  return {
    done() {
      clearInterval(timer);
      clearInterval(sampler);
      const names = new Set(Object.keys(marks).map((k) => k.slice(1)));
      for (const n of names) {
        const s = marks["S" + n], e = marks["E" + n];
        if (s && e) {
          const w = windows.filter((x) => x.t >= s.wall && x.t <= e.wall).map((x) => x.cores).sort((a, b) => a - b);
          if (w.length) console.error(`CORES ${n}: median ${w[w.length >> 1].toFixed(1)}, p90 ${w[Math.floor(w.length * 0.9)].toFixed(1)}, max ${w[w.length - 1].toFixed(1)} (250 ms windows)`);
        }
        if (s && e) console.error(`CPU ${n}: wall ${(e.wall - s.wall).toFixed(2)} s, cpu ${(e.cpu - s.cpu).toFixed(2)} s (${((e.cpu - s.cpu) / (e.wall - s.wall)).toFixed(1)} cores)`);
      }
      console.error(`CPU total: wall ${wall().toFixed(1)} s, cpu ${cpu().toFixed(1)} s`);
    },
  };
};
