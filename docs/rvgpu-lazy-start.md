# rvgpu starts on the first GPU program (VK-15)

`?exec=webgpu` used to start `rvgpu-worker.js` as soon as gpuserver announced its ring, so every desktop boot fetched
16.2 MB of `rvgpu.wasm`, compiled it and kept its code in the tab. Now it starts when a program first connects.

**How:** gpu-exec.js sees `lazy: 1` in gpuserver's announcement and, for `?exec=webgpu` with `navigator.gpu`, writes
`exec_state` 4 into the ring header and spawns nothing. gpuserver sees state 4, writes `rvgpu-0.ready` (so `desktop` still points
programs at it), and on the first accepted connection announces `want: true`; gpu-exec then starts the Worker exactly once
(`started` guards the repeats) and logs "took N ms to answer" (`rvGpu.stats.lazyDelayMs`). A program that connects meanwhile waits
for the caps like it waited for a booting executor (limit 30 s while deferred, 10 s otherwise). Unchanged: the default (vrend)
executor, a browser without `navigator.gpu` (vrend at once), an older gpuserver (no `lazy`), `?rvgpu=eager`. The WebGPU Worker's
"no device" fallback to vrend still works, after the first connection. Check: `node tests/gpu-lazy.js` (6 checks, no browser;
`rvgpu-worker.js` only fetches and compiles on its `start` message, and it gets one).

**Footprint (compiled wasm code, `tools/codemem.sh` on Node 26 / V8 14.6 on the Mac, vmmap rwx ranges; the tab's hard limit
is 4096 MiB, the "Aw, Snap" error 5):**

| what | code committed | ranges |
|---|---|---|
| idle Node | 0 | 0 |
| rvgpu.wasm (16.19 MB, vulkan build) compiled and run: smoke `clear` | 21.3 MiB | 2 |
| same, smoke `draw` (adds compute) | 21.3 MiB | 2 |
| a live node-machine guest (pid of another agent's nativeqt run, 14 CPU-minutes in; `--runtime build/nm-cli/rt`) | 223.7 MiB | 4629 |

So rvgpu costs about 1.3 bytes of code per module byte, a baseline (Liftoff) set: about 0.5% of the 4 GiB, and about a tenth of
a mid-run guest's code. It is small next to what a JIT-heavy guest (Chromium) can reach, so laziness is for boot time, the 16 MB
fetch and the memory a GPU-less session never needs, not a fix for the code-space crash. The guest figure is a snapshot of an
unrelated run and grows with the programs the guest runs; the Chrome numbers are the owner sheet (`the manual test sheets`).
The first-frame cost (fetch, compile, start) is `stats.lazyDelayMs`; the Node smoke's whole `clear` is 0.5 s, the browser's is on the sheet.
