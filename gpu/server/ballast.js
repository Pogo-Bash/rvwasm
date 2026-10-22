// ballast: a test program (gpu/tools/gpuaddr-check.sh), run by gpu/server/boot.js in place of server.js
// (HOSTNODE_HOME=/run/rvgpu RVGPU_LOAD=ballast.js RVGPU_BALLAST_GIB=4.5 /run/hostnode/hostnode). It takes the
// machine's free memory below an address, in blocks it keeps, so that everything the machine hands out after it
// (the GPU server's ring, the window server's table, every client's buffers and pools, the kernel's own pages)
// lies at or above that address: past 2 GiB, where `>> 2` of an address is negative, or past 4 GiB, where it wraps
// to a LOW address, the kernel's. That is what a machine with mem=8192M (the page's default on a big computer)
// does by itself once it has been in use for a while; a test cannot wait for that.
//
// How: from large blocks to small ones, it asks for memory and keeps what comes back below the floor, until four
// asks in a row come back above it (the allocator hands out the lowest free block first); those go back. Every kept
// byte is then written 0xA5, and "ballast: ready" is printed (stderr). When /tmp/ballast.verify exists it reads every
// kept byte again: a changed byte is a write that a program made to an address it did not mean (the wrapped
// addresses of 32-bit operators land here, in the memory this holds) and is told, "ballast: CLOBBERED". It ends when
// /tmp/ballast.stop exists.
(function (gs) {
  "use strict";
  const { sys2, sys6, SYS, log, u8, readFile, env, memory } = gs;
  const PROT_RW = 3, MAP_PRIVATE_ANON = 0x22;
  // (mmap itself, not malloc: the C library keeps small blocks in its own heap and does not ask the kernel for each)
  const alloc = (size) => {
    const a = sys6(SYS.mmap, 0, size, PROT_RW, MAP_PRIVATE_ANON, -1, 0);
    if (a < 0 && a > -4096) {
      throw new RangeError("mmap: error " + -a);
    }
    return a;
  };
  const free = (a, size) => sys2(SYS.munmap, a, size);
  const MiB = 1048576, GiB = 1024 * MiB;
  const floor = Number(env("RVGPU_BALLAST_GIB") || 4.5) * GiB;
  const held = [], taken = [];
  let heldBytes = 0, lowest = Infinity, highest = 0;
  const memFree = () => {
    const m = /MemFree:\s+(\d+) kB/.exec(new TextDecoder().decode(readFile("/proc/meminfo") || new Uint8Array(0)));
    return m ? Number(m[1]) * 1024 : -1;
  };
  const RESERVE = Number(env("RVGPU_BALLAST_RESERVE_MIB") || 256) * MiB;	// (what the kernel needs to go on: it fails otherwise)
  const free0 = memFree();
  // Take everything: from large blocks to small, each size until the kernel has no more of it (or is down to the
  // reserve), then give back the blocks that lie at or above the floor. What the machine has free is then all up there.
  const MIN_SIZE = Number(env("RVGPU_BALLAST_MIN_KIB") || 256) * 1024;
  for (let size = 256 * MiB - 65536; size >= MIN_SIZE; size = Math.floor(size / 2 / 4096) * 4096) {
    const before = taken.length, t0 = Date.now();
    for (let guard = 0; guard < 1000000; guard++) {
      if ((size >= 4 * MiB || guard % 16 === 0) && memFree() < RESERVE + size) {
        break;
      }
      let a;
      try {
        a = alloc(size);
      } catch (error) {
        break;
      }
      taken.push([a, size]);
    }
    log("ballast: size " + size + ": " + (taken.length - before) + " blocks in " + (Date.now() - t0) + " ms, free " + (memFree() / MiB).toFixed(0) + " MiB");
  }
  for (const block of taken) {
    if (block[0] >= floor) {
      free(block[0], block[1]);
    } else {
      held.push(block);
      heldBytes += block[1];
      lowest = Math.min(lowest, block[0]);
      highest = Math.max(highest, block[0] + block[1]);
    }
  }
  log("ballast: took " + taken.length + " blocks of the " + (free0 / MiB).toFixed(0) + " MiB that were free, kept " + held.length
      + " (" + (heldBytes / MiB).toFixed(0) + " MiB below " + floor / GiB + " GiB), gave back " + (taken.length - held.length)
      + "; free now " + (memFree() / MiB).toFixed(0) + " MiB");
  // The reserve the first pass left (the kernel needed it to go on) is still free, low: with the high memory back, take
  // that too, size by size, until three asks in a row come back above the floor.
  for (let size = 128 * MiB - 65536; size >= MIN_SIZE; size = Math.floor(size / 2 / 4096) * 4096) {
    const above = [];
    for (let guard = 0; above.length < 3 && guard < 100000; guard++) {
      let a;
      try {
        a = alloc(size);
      } catch (error) {
        break;
      }
      if (a < floor) {
        held.push([a, size]);
        heldBytes += size;
        lowest = Math.min(lowest, a);
        highest = Math.max(highest, a + size);
      } else {
        above.push(a);
      }
    }
    above.forEach((a) => free(a, size));
  }
  log("ballast: kept " + held.length + " blocks (" + (heldBytes / MiB).toFixed(0) + " MiB) in all; free now " + (memFree() / MiB).toFixed(0) + " MiB");
  const probes = [4096, 65536, 1048576, 4194304 + 20000, 33554432].map((size) => { const a = alloc(size); return [size, a]; });
  probes.forEach(([size, a]) => free(a, size));
  for (const [a, size] of held) {
    u8().fill(0xA5, a, a + size);
  }
  const bad = probes.filter(([size, a]) => size >= MIN_SIZE && a < floor);
  const touch = (name) => {
    const fd = gs.sys4(SYS.openat, -100, gs.path(name, gs.P0), 0o1 | 0o100 | 0o2000000, 0o644);	// O_WRONLY | O_CREAT | O_CLOEXEC
    if (fd >= 0) {
      gs.sys1(SYS.close, fd);
    }
  };
  if (bad.length) {
    log("ballast: FAILED: a block of " + bad.map(([size, a]) => size + " bytes at " + a).join(", ") + " is still below " + floor / GiB + " GiB");
    touch("/tmp/ballast.failed");
    gs.exit(3);
  }
  log("ballast: ready: kept " + held.length + " blocks, " + (heldBytes / MiB).toFixed(0) + " MiB between " + lowest + " and "
      + highest + ", below " + floor / GiB + " GiB; the next blocks of 4 KiB, 64 KiB, 1 MiB, 4 MiB, 32 MiB lie at "
      + probes.map(([, a]) => a).join(" "));
  touch("/tmp/ballast.ready");

  const sleeper = new Int32Array(new SharedArrayBuffer(4));
  const exists = (name) => readFile(name) !== null;
  const verify = () => {
    let changed = 0, first = -1, blocks = 0;
    for (const [a, size] of held) {
      const words = new Uint32Array(memory.buffer, a, size / 4);
      let bad = 0;
      for (let i = 0; i < words.length; i++) {
        if (words[i] !== 0xA5A5A5A5) {
          bad++;
          if (first < 0) {
            first = a + 4 * i;
          }
        }
      }
      if (bad) {
        changed += bad;
        blocks++;
      }
    }
    log(changed ? "ballast: CLOBBERED: " + changed + " words changed in " + blocks + " kept blocks, the first at " + first
                : "ballast: intact: " + (heldBytes / MiB).toFixed(0) + " MiB of kept memory unchanged");
  };
  for (;;) {
    if (exists("/tmp/ballast.verify")) {
      verify();
      gs.sys3(gs.SYS.unlinkat, -100, gs.path("/tmp/ballast.verify", gs.P0), 0);
    }
    if (exists("/tmp/ballast.stop")) {
      gs.exit(0);
    }
    Atomics.wait(sleeper, 0, 0, 300);
  }
})
