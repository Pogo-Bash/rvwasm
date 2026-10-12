// What the kernel stacks of a machine look like, read out of its memory from the page (tools/desktop-drive.js
// eval:FILE, or the DevTools console): every task's stack page, whether the word at its end is still the magic
// number the kernel put there, how far down the stack has been written (stacks start out zeroed, so the lowest
// nonzero word is a lower bound of the deepest use), and whose stack lies right above whose.
// The offsets into task_struct are found by looking, so that this does not depend on the kernel's configuration.
(() => {
  const buf = linuxOs.memory.buffer;
  const m = new DataView(buf), u8 = new Uint8Array(buf);
  const u64 = (a) => Number(m.getBigUint64(a, true));
  const str = (a, n) => { let s = ""; for (let i = 0; i < n && u8[a + i]; i++) s += String.fromCharCode(u8[a + i]); return s; };
  const out = [];
  const MAGIC = 0x57ac6e9dn, STACK = 136;
  const size = buf.byteLength;
  const init_line = linuxLog.find((l) => /init_task (\d+)/.test(l));
  const INIT = Number(init_line.match(/init_task (\d+)/)[1]);
  // The stack's size: init_task's stack is init_stack, and the pt_regs of a task end where its stack ends. Ask a kernel thread.
  let COMM = -1;
  for (let o = 0; o < 4096; o++) if (str(INIT + o, 7) == "swapper") { COMM = o; break; }
  let TASKS = -1;
  for (let o = 0; o + 16 <= 4096 && TASKS < 0; o += 8) {
    const next = u64(INIT + o), prev = u64(INIT + o + 8);
    if (next > 0x10000 && next < size - 4096 && next != INIT + o && prev > 0x10000 && prev < size - 4096 && u64(next + 8) == INIT + o && str(next - o + COMM, 16) == "init") TASKS = o;
  }
  const leaders = [];
  for (let t = u64(INIT + TASKS) - TASKS, n = 0; t != INIT && n < 20000; t = u64(t + TASKS) - TASKS, n++) leaders.push(t);
  let PID = -1;
  for (let o = 0; o + 8 <= 4096 && PID < 0; o += 4) {
    // (pid and tgid, one after the other: 1 and 1 for init, 2 and 2 for kthreadd, 3 and 3 for the task made next.)
    if ([0, 1, 2].every((i) => m.getInt32(leaders[i] + o, true) == i + 1 && m.getInt32(leaders[i] + o + 4, true) == i + 1)) PID = o;
  }
  // signal and thread_node: a pointer in the task to something whose list at +16 points back into the task.
  let SIGNAL = -1, NODE = -1;
  for (let o = 0; o + 8 <= 4096 && SIGNAL < 0; o += 8) {
    const p = u64(leaders[0] + o);
    if (p > 0x10000 && p < size - 64 && p % 8 == 0 && (p < leaders[0] || p > leaders[0] + 4096) && m.getInt32(p + 8, true) == 1) {
      const a = u64(p + 16), b = u64(p + 24);
      if (a == b && a > leaders[0] && a < leaders[0] + 4096 && u64(a) == p + 16 && u64(a + 8) == p + 16) { SIGNAL = o; NODE = a - leaders[0]; }
    }
  }
  const tasks = [];
  for (const leader of leaders) {
    const head = u64(leader + SIGNAL) + 16;
    let n = 0;
    for (let node = u64(head); node != head && n < 5000; node = u64(node), n++) tasks.push(node - NODE);
    if (n == 0) tasks.push(leader);
  }
  // How large a stack is: the distance from a stack's page to the next higher thing is not known, so take it from
  // the kernel's own arithmetic: pt_regs (80 bytes) end at the stack's end, and a kernel thread's cpuflags there is 1.
  let THREAD = 4096;
  const kthreadd = leaders[1];
  for (const guess of [4096, 8192, 16384, 32768, 65536]) {
    if (u64(u64(kthreadd + STACK) + guess - 80 + 8) == 1) { THREAD = guess; break; }
  }
  out.push(`${leaders.length} processes, ${tasks.length} tasks; stacks of ${THREAD} bytes; offsets: comm ${COMM} tasks ${TASKS} pid ${PID} signal ${SIGNAL} thread_node ${NODE}`);
  const info = (t) => {
    const stack = u64(t + STACK);
    let low = -1;
    for (let o = 8; o < THREAD; o += 8) if (m.getBigUint64(stack + o, true) != 0n) { low = o; break; }
    return { t, pid: m.getInt32(t + PID, true), comm: str(t + COMM, 16), cpu: m.getUint32(t, true), pool: m.getInt32(t + 12, true), stack,
             magic: m.getBigUint64(stack, true), low, flags: m.getBigUint64(stack + THREAD - 80 + 8, true),
             sp: m.getBigUint64(stack + THREAD - 80, true), nr: m.getInt32(stack + THREAD - 80 + 16, true), ret: m.getBigInt64(stack + THREAD - 8, true) };
  };
  const all = tasks.map(info);
  const by_stack = new Map(all.map((i) => [i.stack, i]));
  const show = (i) => `task ${i.pid} (${i.comm}) cpu ${i.cpu}${i.pool ? " pool" : ""} stack 0x${i.stack.toString(16)} end word 0x${i.magic.toString(16)}${i.magic == MAGIC ? "" : " CLOBBERED"}, lowest nonzero word at +${i.low} (at least ${THREAD - i.low} bytes used), pt_regs: sp 0x${i.sp.toString(16)} cpuflags 0x${i.flags.toString(16)} nr ${i.nr} ret ${i.ret}`;
  const bad = all.filter((i) => i.magic != MAGIC);
  out.push(`tasks whose stack end is clobbered: ${bad.length}`);
  for (const i of bad) {
    out.push("  " + show(i));
    const below = by_stack.get(i.stack - THREAD);
    out.push("    right below that stack: " + (below ? show(below) : "not a task's stack"));
  }
  const odd = all.filter((i) => i.pool && i.flags != 3n);
  out.push(`pool tasks whose saved cpuflags are not 0x3 (user mode, interrupts on): ${odd.length}`);
  for (const i of odd.slice(0, 20)) {
    out.push("  " + show(i));
    const above = by_stack.get(i.stack + THREAD);
    out.push("    right above that stack: " + (above ? show(above) : "not a task's stack"));
  }
  const deep = all.slice().sort((a, b) => a.low - b.low);
  out.push("deepest stacks:");
  for (const i of deep.slice(0, 8)) out.push("  " + show(i));
  const hist = {};
  for (const i of all) { const k = Math.floor((THREAD - i.low) / 512) * 512; hist[k] = (hist[k] || 0) + 1; }
  out.push("bytes used (lower bound), tasks: " + Object.keys(hist).map(Number).sort((a, b) => a - b).map((k) => k + "+:" + hist[k]).join(" "));
  let adjacent = 0;
  for (const i of all) if (by_stack.has(i.stack - THREAD)) adjacent++;
  out.push(`tasks whose stack has another task's stack (its pt_regs) right below it: ${adjacent} of ${all.length}`);
  const panic = linuxLog.find((l) => /Kernel panic/.test(l));
  if (panic) {
    out.push(panic.slice(0, 900));
    const k = panic.match(/task (\d+) \(/);
    const victim = k && all.find((i) => i.pid == Number(k[1]));
    if (victim) {
      out.push("the task that panicked: " + show(victim));
      const above = by_stack.get(victim.stack + THREAD);
      out.push("  right above its stack: " + (above ? show(above) : "not a task's stack; that page starts with 0x" + m.getBigUint64(victim.stack + THREAD, true).toString(16)));
      const w = []; for (let o = THREAD - 128; o < THREAD; o += 8) w.push(m.getBigUint64(victim.stack + o, true).toString(16));
      out.push("  the top 128 bytes of its stack page: " + w.join(" "));
    }
  }
  // What the kernel last said on the console (a warning before a panic is as telling as the panic).
  if (panic && typeof output == "string") {
    out.push("the console's last lines:\n" + output.slice(-2500).replace(/\x1b\[[0-9;?]*[a-zA-Z]/g, "").split("\n").slice(-25).join("\n"));
  }
  return out.join("\n");
})()
