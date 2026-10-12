// What ran as each CPU, read out of a machine from the page (tools/desktop-drive.js eval:FILE or --on-hang FILE,
// or the DevTools console): the kernel's notes (arch/wasm/kernel/traps.c: a ring per CPU of who passed which of the
// kernel's doors, with what the scheduler held to be the CPU's current task at that moment), the report the kernel
// wrote when it panicked, and the main thread's book of task switches (tools/patch-runtime.py, 8), which is the one
// place where the order of all the Workers' switches is known. Addresses of the kernel's variables come from
// kernel-symbols.txt (tools/stage.sh).
(async () => {
  const out = [];
  const os = window.linuxOs;
  if (!os || !os.memory) { return "the page does not show its machine's memory"; }
  const at = {};
  for (const line of (await (await fetch("kernel-symbols.txt", { cache: "no-store" })).text()).split("\n")) {
    const p = line.split(" ");
    if (p.length == 3) { at[p[2]] = parseInt(p[0], 16); }
  }
  const buf = os.memory.buffer;
  const m = new DataView(buf), u8 = new Uint8Array(buf);
  const str = (a, n) => { let s = ""; for (let i = 0; i < n && u8[a + i]; i++) s += String.fromCharCode(u8[a + i]); return s; };
  const name = (t) => (os.runner_names && os.runner_names[t]) || "?";
  const now = performance.now();

  // The kernel's report, if it wrote one.
  if (at.wasm_flags_report !== undefined) {
    const report = str(at.wasm_flags_report, 8192);
    out.push("---- the kernel's report ----\n" + (report || "(none)"));
  }

  // The main thread's complaints, and what Workers said about crashes and panics.
  const complaints = (window.linuxLog || []).filter((l) => /\[Main\]: (serialize_tasks|create_and_run_task|release_task|task \d+ .* is made)/.test(l));
  out.push("---- what the main thread found odd (" + complaints.length + " lines) ----\n" + complaints.slice(-30).join("\n"));
  if (os.kernel_crashes) {
    out.push("---- Workers that threw inside the kernel (" + os.kernel_crashes.length + ") ----\n" + os.kernel_crashes.slice(-10).map((l) => l.slice(0, 400)).join("\n"));
  }
  const crashes = (window.linuxLog || []).filter((l) => /Wasm crash|Kernel panic|Not the current|Trying to stop|Tried to stop|raise_exception|woken for a switch/.test(l));
  out.push("---- crashes and panics in the log (" + crashes.length + " lines) ----\n" + crashes.slice(-20).map((l) => l.slice(0, 700)).join("\n"));

  // The rings: every CPU's last notes, the newest 12 of each, newest last.
  if (at.wasm_flags_changes !== undefined && at.__per_cpu_offset !== undefined) {
    const SITES = { 1: "syscall enter", 2: "syscall exit", 3: "first run", 4: "irq enter", 5: "irq exit", 6: "switch out ->", 7: "switched in <-", 8: "idle woke, irqs", 9: "exception", 10: "new task's flags" };
    const lines = [];
    for (let cpu = 0; cpu < 64; cpu++) {
      const base = Number(m.getBigUint64(at.__per_cpu_offset + 8 * cpu, true));
      const n = m.getUint32(base + at.wasm_flags_changed, true);
      if (!n) { continue; }
      const flags = at.wasm_cpuflags !== undefined ? m.getUint32(base + at.wasm_cpuflags, true) : -1;
      lines.push(`cpu ${cpu}: flags 0x${flags.toString(16)}, ${n} notes`);
      for (let i = Math.max(1, n - 11); i <= n; i++) {
        const e = base + at.wasm_flags_changes + (i % 32) * 40;
        const pid = m.getInt32(e, true), site = m.getInt32(e + 4, true), curr = m.getInt32(e + 8, true), aux = m.getInt32(e + 12, true);
        const f = m.getUint32(e + 16, true), on_cpu = m.getUint32(e + 20, true), task = Number(m.getBigUint64(e + 24, true));
        const when = Number(m.getBigUint64(e + 32, true)) / 1e6;  // ns -> ms, the page's clock
        lines.push(`  #${i} ${(now + performance.timeOrigin - when).toFixed(1)} ms ago: task ${pid} [${task}] (${name(task)}) on_cpu ${on_cpu}, ${SITES[site] || site} ${aux}, scheduler's current ${curr}${curr != pid && site != 6 ? "  <- NOT THE CURRENT TASK" : ""}, flags after 0x${f.toString(16)}`);
      }
    }
    out.push("---- the kernel's notes per CPU ----\n" + lines.join("\n"));
  }

  // The book: the last switches, with the time before now.
  if (os.switches) {
    const WHAT = { S: "switch", C: "switch to new", R: "release", K: "worker ended (was running:", U: "cpu up", X: "cpu stopped", D: "dead task's last switch" };
    const tail = os.switches.slice(-120);
    out.push("---- the main thread's last " + tail.length + " switches of " + os.switches.length + " kept ----\n"
             + tail.map(([t, w, a, b]) => `${(now - t).toFixed(1)} ms ago: ${WHAT[w] || w} ${a} (${name(a)})${w == "K" ? " " + b + ")" : w == "R" || w == "X" || w == "D" ? "" : " -> " + b + " (" + name(b) + ")"}`).join("\n"));
  }
  return out.join("\n");
})()
