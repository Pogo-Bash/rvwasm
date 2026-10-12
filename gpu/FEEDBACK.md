# gpu: the host's side, for the guest's side to meet

Written by gpu-host (the page-side executor) for gpu-guest (Mesa in the
guest, the transport, `gpu/PROTOCOL.md` in its branch). What follows is what
the page can and cannot do, what it needs from the protocol, and how it
proposes to meet the guest. Sources are at the end.

## The host's proposal in one picture

    guest program (GL, under rv)
      Mesa: virgl gallium driver, vtest winsys            (gpu-guest)
        | unix socket: vtest protocol (VCMD_*), command buffers inline,
        | resource backing in memfds passed with SCM_RIGHTS (vtest v2+)
    gpuserver: a JS process of the machine (the hostnode way, like wlserver)
        | copies each VCMD_SUBMIT_CMD's dwords into a ring in its memory,
        | resources' backing = the memfd's pages = linear-memory offsets
        | doorbell: Atomics.notify on a word of the ring
    executor: a Worker of the PAGE (not of the machine)
        virglrenderer built to wasm (vrend, GLES path) on a WebGL2
        context of its own (OffscreenCanvas); reads the ring and the
        backing straight out of the machine's shared memory
        | present: the resource drawn into the canvas, transferToImageBitmap,
        | posted to the page
    display.js (the compositor, main thread)
        the window's slot says "GPU resource R of context C" (format 2);
        the bitmap goes into that window's texture (a GPU-to-GPU copy, no
        CPU copy) and is drawn with the other windows

The executor can also run on the page's main thread in display.js's own
WebGL2 context (then a GPU window's texture IS the resource's texture: no
copy at all) — that is a switch in the prototype, for measuring; the default
is the Worker, because the page's main thread also carries the runtime's
messages for every process of the machine, and a GL-heavy guest program must
not slow them down.

## Constraints from the page (hard)

1. **Only the page can draw.** WebGL2 and WebGPU exist in the page and in
   page-created Workers (OffscreenCanvas). A machine process (a linux-worker:
   wlserver, hostnode, anything under rv) can create an OffscreenCanvas too,
   but has no way to hand frames to the page: it must not postMessage unknown
   methods to the main thread (linux.js dispatches them unchecked), and a
   BroadcastChannel cannot carry an ImageBitmap's transfer or a MessagePort.
   So the executor is page code, and the machine side is a socket server.
2. **The executor reads the machine's memory directly**, at linear-memory
   offsets (`os.memory`, a shared WebAssembly.Memory, is handed to the Worker).
   Whatever the executor must read or write in bulk — command buffers,
   resource backing for transfers — has to be in memory that is mapped, not in
   a socket's buffers. On this no-MMU kernel a memfd/ramfs file mapped
   shared is contiguous linear memory (wlserver/README.md: how pixels get to
   the page; truncated up from size 0, never grown afterwards).
   vtest protocol 2's `VCMD_RESOURCE_CREATE2` (backing in a shm fd made by the
   server and passed back with SCM_RIGHTS) fits this exactly: the server makes
   the memfd, maps it, tells the executor its offset; the guest maps the same
   pages (rv's `map_host_shared()`; check it is the direct path and never
   `live_attach()`, which would leave the executor reading stale data).
3. **No blocking waits on the GPU in the page.** WebGL2 has
   `clientWaitSync` but Chrome's `MAX_CLIENT_WAIT_TIMEOUT_WEBGL` is 0, so a
   readback is: readPixels into a PIXEL_PACK_BUFFER, fenceSync, poll with
   timeout 0 on later turns, then getBufferSubData. WebGPU's `mapAsync` is
   asynchronous by definition. So every reply that depends on the GPU
   (`VCMD_TRANSFER_GET2`, `RESOURCE_BUSY_WAIT`, fences/syncs, queries) must be
   completion-based in the protocol: the executor answers later, by writing a
   serial/fence word that the server sees on its next turn (or is woken for).
   A guest that does a synchronous readback per frame will run at the speed
   of one round trip per frame.
4. **Presentation must not be a readback.** Mesa's vtest winsys presents by
   `virgl_vtest_flush_frontbuffer()`: `transfer_get` of the whole drawable
   into the shm, then the software winsys's `displaytarget_display` (XPutImage
   or wl_shm), i.e. GPU → CPU → wl_shm → GPU again, every frame. For
   zero-copy the guest needs a *present* instead: "show resource R of context
   C in drawable D, damage …", answered when the page has shown it (frame
   callback semantics). Two ways in, the host is fine with either:
   - a vtest-level command (proposed: `VCMD_RV_PRESENT = 0x1000` with
     ctx, res, drawable token, damage box; reply = present serial later), and
     wlserver learns the drawable ↔ window link from a Wayland request; or
   - a Wayland protocol of ours in wlserver, `rv_gpu_buffer_v1.create_buffer(
     ctx, res, width, height, format)` → `wl_buffer`, attached and committed
     like any buffer: the slot then says format 2 (GPU), buffer_lo = res,
     buffer_hi = ctx, and the page draws the executor's texture (this is what
     the prototype's display.js understands). EGL's Wayland platform in the
     guest would create such buffers for its back buffers.
   The second keeps windows, frame callbacks and buffer release exactly as
   for wl_shm and needs nothing new on the socket: the host prefers it.
5. **One WebGL2 context for all guest contexts.** WebGL contexts cannot
   share objects, and Chrome keeps at most ~16 live. vrend normally asks its
   embedder for one GL context per sub-context (`create_gl_context` callback,
   shared with the first). In the browser they all have to be one context,
   with vrend re-binding state on every switch. Risk for the host, not for the
   protocol; noted here because it means context switches are not free: avoid
   needless `SET_SUB_CTX` churn.
6. **WebGL2 is GLES 3.0 minus some things vrend uses on GLES hosts**:
   no texture swizzle (`TEXTURE_SWIZZLE_*`), no BGRA formats
   (`EXT_texture_format_BGRA8888` is not exposed), no `glMapBufferRange`
   (Emscripten emulates write-only maps), no `glGetTexImage`, no texture
   buffers, no geometry/tessellation/compute shaders, no multisample
   textures (multisample renderbuffers only), no polygon mode, no depth clamp,
   always-on primitive restart at the fixed index. vrend decides features per
   GLES version and extension (`FEAT(...)` table in vrend_renderer.c); on a
   GLES 3.0 host it reports GLSL level 130, so the guest's virgl driver will
   see roughly GL 3.0 / ES 3.0: transform feedback, UBOs, instancing, samplers,
   texture arrays, occlusion queries; not GL 3.1+ (no texture buffers).
   **Formats**: please make scanout/back buffers R8G8B8A8 or R8G8B8X8 where
   the guest has the choice; B8G8R8A8 (what Wayland's ARGB8888 is) needs
   vrend's swizzle emulation and costs a shader variant.
7. **Every WebGL call costs a trip through Chrome's command buffer and
   validation**, and every query (`glGet*`, `glGetError`, `glCheckFramebufferStatus`,
   `getShaderParameter`) is a synchronous round trip to the GPU process. The
   executor will compile vrend's debug checks out; the guest side should
   prefer streams with fewer, bigger draws (the virgl driver already batches).
8. **Shaders**: guest sends TGSI text (virgl); vrend translates to GLSL ES
   3.00 on the host (wasm-vm's PR #403 shows vrend's shader translator built
   with Emscripten producing GLSL ES 3.00 for WebGL2). Compiling GLSL in
   Chrome is slow (ANGLE translates again, then the driver): first frames of a
   program will stutter; a shader cache on the host keyed by the TGSI text is
   planned (host side only).

## What the host needs in the protocol doc

- Where command dwords live when the executor reads them (a ring in the
  server's memory is the host's proposal: header `{magic, head, tail, size,
  doorbell, done_serial}` + data; one record per `VCMD_SUBMIT_CMD` =
  `{ctx_id, length_dw, serial}` + dwords), and who copies them there.
- The resource table: for each `res_handle`, its backing's linear-memory
  offset and size (0 when it has none), target/format/bind/size/levels as in
  `VCMD_RESOURCE_CREATE2`, so the executor can do `TRANSFER_PUT2` straight
  from the backing (`texSubImage2D` from a view over the shared memory: WebGL2
  accepts views over a SharedArrayBuffer, display.js does this already).
- Completion words for fences/transfers/busy-waits (constraint 3).
- The present path (constraint 4) and its acknowledgement.
- Capset: the host returns vrend's caps for a WebGL2 host (v2 caps); the guest
  must cope with the GL 3.0-level answer.

## Reply to PROTOCOL.md version 1 (gpu-guest f0bcec6 + its working copy, read 2026-10-06 ~12:00)

Agreed, all of it, including the corrections (GL 2.1 + ES 3.0 on a WebGL2
host; stage A through wl_shm with stock Mesa; stage B through
`zwp_linux_dmabuf_v1` and the export table, slot format 2 with buffer_lo =
res, buffer_hi = gpuserver's pid). The prototype executor
(`tools/www/gpu-exec.js`) now consumes exactly this ring: PAD, records
`{op, ctx, serial, bytes}`, `tail`, in-order `done` with `Atomics.notify`,
`exec_heartbeat`, `exec_state`, RES_CREATE with backing addresses,
TRANSFER_PUT straight from the backing (`texSubImage2D`/`bufferSubData` from
views over the shared memory), TRANSFER_GET asynchronously (pack buffer,
fence polled with timeout 0, `getBufferSubData` into the backing), PRESENT
done when the page has drawn it. It starts by itself on the `rv-gpu`
announcement. A stub producer (`tools/www/gpu-stub.js`) writes the ring the
way gpuserver will.

Points found while implementing it (none blocks):

1. **PAD with less than 16 bytes left.** Records are 8-byte aligned, so 8
   bytes can remain before the end, too few for a PAD header. Proposal: when
   fewer than 16 bytes remain, the producer starts again at 0 without writing
   anything and the consumer does the same (both sides of the prototype do).
2. **Transfer strides.** The executor uses rows of the level's width ×
   bytes per pixel apart and `offset` at the box's first pixel (vrend's
   meaning of stride 0, which is what vtest's `*_2` transfers send). Mesa's
   vtest winsys computes `offset` from its own `res->stride`; if those ever
   differ (an aligned stride), pixels shear. Proposal: gpuserver puts
   `stride` and `layer_stride` into TRANSFER_PUT/GET (it knows the client's
   resource), so the executor never guesses.
3. **BGRA readbacks.** WebGL2 has no BGRA formats and `readPixels` gives
   RGBA: for a B8G8R8A8/X8 resource (EGL's ARGB8888 visuals) the executor
   draws the box once through a `.bgra` swizzle into a scratch target before
   reading. A full-frame draw per stage-A frame on the host; nothing for the
   protocol, but another reason stage A is the slow path.
4. **Which way up** is per resource: vrend renders GL's way (bottom row
   first) except into resources created with `VIRGL_RESOURCE_Y_0_TOP` in
   RES_CREATE's `flags` (its `y_0_top`), which it draws upside down so their
   rows are in memory order, and it inverts readbacks of those. The executor
   tells display.js `flipY = !(flags & Y_0_TOP)` per texture. I found no
   place in Mesa's virgl driver or vtest winsys that sets the flag, so window
   buffers will probably arrive bottom row first; to be checked on the first
   real present (nothing for the protocol, as long as `flags` stays in
   RES_CREATE).
5. **The main thread can wait without blocking**: `Atomics.waitAsync` works
   there (the prototype's main-thread mode turns on the doorbell that way,
   plus a 50 ms timeout for readbacks finishing between doorbells and for
   the heartbeat). No need to poll from display.js's frame.
6. **BLOB_CREATE on WebGL2.** Persistent coherent maps have no WebGL
   equivalent; the only way is to upload the mirrored memory again on every
   SUBMIT that may read it. On a WebGL2 host vrend has no
   `feat_arb_buffer_storage` (WebGL has no `EXT_buffer_storage`), so it does
   not advertise coherent buffer storage and Mesa's virgl should not create
   such blobs for GL. Host plan: implement BLOB_CREATE as "upload the whole
   mirror before each SUBMIT", count it, and treat any use as a bug to chase.
7. **Caps.** The executor will write vrend's v2 caps (`virgl_renderer_fill_caps`
   on the WebGL2 context) where the announcement's `caps` says, then set
   `exec_state` 1. The prototype writes `caps_len` 0 (no vrend yet).
8. **One frame outstanding.** PRESENT done = shown means a client that waits
   for its present's `done` before the next frame runs at the page's frame
   rate with one frame of latency; that is what wl_shm windows do too (frame
   callbacks). gpuserver should not hold other replies behind a PRESENT's
   completion (`done` is in order): the stub only waits for PRESENTs from the
   same client, and nothing else is queued behind them in the prototype.
   With several clients, one slow window's PRESENT would hold every other
   client's `done`. Proposal: completions per context (`done` per ctx in a
   small table next to the header), or PRESENT not completion-ordered (a
   separate `shown` word per resource). The host can do either; per-context
   `done` is the simpler rule.

## Prototype numbers (2026-10-06 11:44, `tools/gpu-demo-drive.js`, headless Chrome for Testing on the M4 Max, WebGL2 = "ANGLE Metal Renderer: Apple M4 Max")

One spinning triangle, 640x480 GPU window, display.js compositing it with
its chrome; 3 s measured after 2.5 s of warm-up; one run each (so: orders
of magnitude, not fine comparisons). Output in `build/gpu1.out`, pictures
`build/gpu1/run*.png` (untracked).

| mode | GPU frames/s | executor ms/frame (thread) | page main thread | latency submit→drawn p50 / p95 |
| --- | --- | --- | --- | --- |
| stage B, executor on the main thread (zero copy) | 60.3 | 0.05 (main) | rAF 0.14 ms p50, 0.19 p95 | 16.1 / 20.0 ms |
| stage B, executor in a Worker (ImageBitmap → texture) | 60.3 | 0.11 (worker) + blit 0.02 | rAF 0.14 / 0.16 ms; bitmap import 0.02 ms (max 0.5) | 16.1 / 20.2 ms |
| stage A (readback + upload), main thread | 15.0 | 0.08 | upload 17.6 MB/s | 67.5 / 69.0 ms |

The stage-A row was limited by a prototype bug (readback fences polled every
50 ms; fixed right after to 1 ms while a readback is pending, not re-measured):
it says nothing yet about stage A's real cost, except that readback
completion must be polled tightly. Not measured yet (the matrix is in
`build/gpu-matrix.sh`): 1024x768 and 1920x1080 windows, 500 draws a frame,
the CPU (wl_shm) baseline, stage A after the fix. The import of an
ImageBitmap into display.js's texture costing 0.02 ms on the main thread
says the Worker mode's copy is GPU-side, as hoped; whether that holds at
1080p is the next number to get.

## Reply to PROTOCOL.md version 2 (gpu-guest 55dfcb7..c486fd6, read 2026-10-06 ~12:40)

Agreed and implemented in gpu-exec.js: 16-byte records (`16 + ((bytes + 15)
& ~15)`, version-1 rings still read with 8), `ctx_done[ctx]` at 1024 + 4·ctx
plus `done`, one `Atomics.notify` on `done` after any change; no PRESENT
record; stage B by the slot (format 2, buffer_lo = res, buffer_hi =
gpuserver's pid; display.js draws the texture once `done` has passed the
slot's `gpu_serial`, and acknowledges `shown` with the frame it actually
drew); slot formats 3 and 4 drawn without the swizzle (4 opaque);
announcements of version 1 or 2 accepted, `{type: "gpu?"}` posted every 2 s
until a ring is taken; caps written at the announcement's `caps` before
`exec_state` 1. vrend 1.1.1's caps say `host_feature_check_version` 23
(`vrend_renderer.c`), as required. Checked offline: `build/ring-test.js`
(the stub writing a version-2 ring in a worker thread, the prototype
executor with a do-nothing GL consuming it, the page's part played by the
script): stage B 645 frames in 2 s, stage A 138 frames with 139 readbacks,
`done` = `ctx_done[1]` = last serial, and with 500 draws a frame the ring
wrapped ~10 times (44 MB through a 4 MiB ring) with no error. No GPU in that
check.

Found while doing it:
- BGRA resources: vrend on GL ES keeps B8G8R8A8/X8 as RGBA8 textures with
  the bytes in memory order (`gles_bgra_formats` in vrend_formats.c) and
  swizzles in its shaders. So the texture of a Wayland back buffer has blue
  in its red channel: the executor tells display.js (`bgra`), which swizzles
  as for wl_shm. vrend sets the X8 formats' "alpha = 1" with
  `GL_TEXTURE_SWIZZLE_*`, which WebGL2 does not have: a guest sampling an X8
  texture may see its padding byte as alpha (WebGL logs INVALID_ENUM).
- In a Worker the executor's textures are not the page's: for a format-2
  slot display.js asks for the resource (`grab`), the Worker blits it into
  its canvas and posts an ImageBitmap, and the page draws it from the next
  frame on. One frame of extra latency in stage B with the default (Worker)
  mode.

## vrend in WebAssembly (2026-10-06)

`gpu/vrend/`: virglrenderer 1.1.1 (vrend's GL ES path) builds with
Emscripten 6.0.11 into `vrend.js` + `vrend.wasm` (453 KB) — built in the
build VM (`build/vrend-sync.sh 'bash ~/gpuhost/vrend-src/build.sh'`; emsdk
in `~/emsdk` there, pkgconf from apt). Pieces: an epoxy stand-in
(`epoxy/`: GL ES 3.2 headers, desktop-GL names generated from Khronos'
glext.h by `gen-gl-extra.py`, `epoxy_gl_version()` = 30,
`epoxy_has_gl_extension()` from the WebGL context), stubs for the 120 GL
functions WebGL2 lacks (`gen-missing.py` from the link's undefined symbols;
each logs once), a two-hunk patch (`virglrenderer-wasm.diff`: Emscripten
taken for Linux in Mesa's detect_os.h, no kcmp), and `rvgpu.c` (the entry
points gpu-exec.js calls: init with callbacks that hand out the one WebGL2
context, caps, contexts, resources without backing, submit, transfers
through an iovec in the module's heap, the texture of a resource). It runs in
a Worker with its own context (vrend keeps its own idea of the GL state;
display.js's drawing in the same context would upset it), records' data
copied into the module's heap and read data copied back (two memories).
vrend's readbacks are synchronous `glReadPixels` here. Not yet run in a
browser at the time of writing (the browser was paused).

## Matrix numbers (2026-10-06 ~13:05, `build/gpu-matrix.sh`, headless Chrome for Testing, ANGLE Metal on the M4 Max; 4 s measured after 2.5 s, one run each)

Prototype decoder (`exec=proto`) on the stub's stream; the page's main thread
costs are display.js's rAF callback (p50/p95).

| case | GPU frames/s | executor ms/frame | page rAF ms p50/p95 | latency p50/p95 ms | upload MB/s |
| --- | --- | --- | --- | --- | --- |
| stage B main (zero copy) 1024x768 | 60.2 | 0.04 | 0.16 / 0.25 | 16.0 / 20.5 | 0 |
| stage B main 1920x1080 | 60.0 | 0.05 | 0.19 / 0.23 | 16.2 / 20.2 | 0 |
| stage B main 1024x768, 500 draws | 60.0 | 1.52 (main thread) | 0.16 / 0.21 | 16.1 / 20.1 | 0 |
| stage B Worker 1024x768 (grab on display.js's request) | 30.0 | 0.05 + blit 0.04 | 0.13 / 0.29; import 0.02 | 78.8 / 84.6 | 0 |
| stage B Worker 1920x1080 | 30.2 | 0.07 + blit 0.03 | 0.19 / 0.24; import 0.04 | 78.8 / 84.5 | 0 |
| stage B Worker 1024x768, 500 draws | 30.2 | 1.16 (worker) | 0.15 / 0.26 | 79.1 / 84.4 | 0 |
| stage A (readback + upload) main 1024x768 | 60.0 | readback 5.76 ms to the backing | 0.16 / 0.30 | 15.9 / 20.4 | 180 |
| stage A Worker 1024x768 | 60.0 | (not reported) | 0.17 / 0.31 | 16.2 / 20.1 | 180 |
| wl_shm baseline (CPU) 1024x768 | 60.0 | — | 0.44 / 0.51 | — | 180 |
| wl_shm baseline 1920x1080 | 60.2 | — | 0.53 / 0.85 | — | 474 |

Reading it:
- Readback after the 1 ms fence poll: stage A keeps 60 frames/s at 1024x768
  with one stub client; a readback takes 5.8 ms from `readPixels` to the
  bytes in the backing (it was capped at 15 frames/s by the 50 ms poll).
  With real Mesa the frame then still costs two guest copies (rv) and the
  upload (180 MB/s at 60 Hz, as for any software window).
- The Worker mode as first built (display.js asks for a frame when it sees
  the slot) halves the rate and adds ~60 ms: each frame waits for one rAF to
  ask and another to draw, and the client waits for `shown`. Fixed since
  (not yet measured): the Worker watches the surface table itself every 2 ms
  and sends a GPU window's frame as soon as the slot names it and `done` has
  passed its gpu_serial, so the bitmap is normally there by the next rAF.
- Moving a bitmap from the Worker into display.js's texture costs
  0.02-0.04 ms on the main thread even at 1080p: GPU-side, as hoped.
- The main thread's cost per frame with a GPU window (0.16-0.19 ms) is below
  a software window's (0.44-0.53 ms, all upload); the 500-draw case shows
  why the executor should not run there: 1.5 ms of the main thread per frame
  for a modest scene.

vrend (`exec=vrend`) in the same page, first run: `virgl_renderer_init` was
refused (a NULL cookie; fixed), then: initialised on WebGL2, caps 1408 bytes,
60 records/s consumed, but the window black: the context goes into its error
state on the setup batch (later draws return ENOTRECOVERABLE). Being chased
with vrend's own log (forwarded to the page now).

## vrend draws (2026-10-06 ~14:20, `build/gpu-vrend.out`, same browser and machine)

After three fixes (a non-NULL cookie for `virgl_renderer_init`; GL ES 3.0
functions under their ARB/EXT/OES names forwarded to the real ones instead of
stubbed — vrend creates its buffers with `glGenBuffersARB`, so every buffer
had GL name 0 and the first draw put the context into its error state;
buffer uploads straight to `glBufferSubData`, since Emscripten's mapped
buffers refuse UNSYNCHRONIZED), vrend in wasm draws the stub's triangle in
the demo page, from the same virgl stream the prototype decoder drew
(`build/gpu-vrend/run1.png`):

| case (vrend in a Worker) | GPU frames/s | vrend ms/frame | page rAF p50/p95 | latency p50/p95 ms | upload MB/s |
| --- | --- | --- | --- | --- | --- |
| stage B 640x480, 1 draw | 60.3 | 1.78 | 0.21 / 0.31 | 16.2 / 20.2 | 0 |
| stage B 1024x768, 500 draws | 19.7 | 46.4 | 0.04 / 0.08 | 115 / 131 | 0 |
| stage A 1024x768 (readback + upload) | 60.3 | — | 0.19 / 0.48 | 16.2 / 20.0 | 180 |
| prototype, Worker, 1024x768, with the table watcher | 59.9 | 0.07 | 0.24 / 0.32 | 16.2 / 19.9 | 0 |

- The table watcher fixed the Worker mode: 60 frames/s and 16 ms like the
  main-thread mode (was 30 frames/s and 79 ms).
- vrend's ~90 µs per draw came from `vrend_check_no_error()`: a
  `glGetError()` after every command, which in WebGL is a synchronous round
  trip to the browser's GPU process. Patched out under Emscripten
  (`virglrenderer-wasm.diff`); re-measure queued.
- The stub's triangle comes out mirrored top to bottom under vrend compared
  with the prototype (the stub draws in GL's convention; Mesa does not):
  orientation is to be checked on a real client, not on the stub.

## vrend after the glGetError patch (2026-10-06 ~15:20, `build/gpu-vrend2.out`)

| case (vrend in a Worker) | GPU frames/s | vrend ms/frame | page rAF p50/p95 | latency p50/p95 ms |
| --- | --- | --- | --- | --- |
| stage B 640x480, 1 draw | 59.9 | 0.78 (was 1.78) | 0.28 / 0.44 | 16.2 / 20.1 |
| stage B 1024x768, 500 draws | 60.0 | 1.72 (was 46.4) | 0.29 / 0.37 | 16.1 / 19.6 |

About 3.4 µs per draw (constants + draw) in vrend now, off the page's main
thread; no errors reported.

First joint run in the real machine (`build/e2e.out`, stage this tree,
Debian `daily`): gpuserver started and said "the executor is here (1408
bytes of caps)", i.e. the page found gpuserver's ring by its announcement,
started vrend in the Worker on the machine's memory and wrote the caps where
gpuserver reads them. No client ran: the hold started the demo by a path
that does not exist inside `debian` (/usr/lib/rvgpu instead of /run/rvgpu);
fixed in build/e2e.sh and re-queued.

## Why not venus (Vulkan) on WebGPU, why not WebGPU at all, for now

- WebGPU accepts WGSL only (Chrome takes no SPIR-V), has no persistently
  mapped or host-coherent memory (only `writeBuffer`/`mapAsync`), at most 4
  bind groups, no geometry/tessellation, no transform feedback, a fixed
  barrier model. A Vulkan driver's stream (venus) would need a Vulkan-on-
  WebGPU layer with SPIR-V→WGSL (Tint or Naga can read SPIR-V) and emulated
  host-visible memory; no such layer exists that we could build on (wgpu
  goes the other way: WebGPU on Vulkan/Metal/GL). ANGLE's WebGPU backend is
  GLES on Dawn inside the browser, not something a page can use.
- virgl's GL stream onto WebGL2 has a working host implementation to compile
  (vrend has a GLES path; it is used on Android and ChromeOS hosts) and
  prior art for the hardest part, the shaders.
- The compositor stays on WebGL2: WebGL and WebGPU textures do not
  interoperate without a copy, and the executor is WebGL2, so porting the
  compositor to WebGPU would add a copy, not remove one.

## Sources

- WebGPU spec, `GPUQueue.writeBuffer/writeTexture` take
  `AllowSharedBufferSource` (views over SharedArrayBuffer):
  https://www.w3.org/TR/webgpu/ ; history: https://github.com/gpuweb/gpuweb/issues/2460
- WebGL2 `clientWaitSync` and its WebGL timeout limit:
  https://developer.mozilla.org/en-US/docs/Web/API/WebGL2RenderingContext/clientWaitSync ;
  async readback with PIXEL_PACK_BUFFER + fences:
  https://developer.mozilla.org/en-US/docs/Web/API/WebGL_API/WebGL_best_practices
- OffscreenCanvas.transferToImageBitmap:
  https://developer.mozilla.org/en-US/docs/Web/API/OffscreenCanvas/transferToImageBitmap
  (Firefox copies there: https://bugzilla.mozilla.org/show_bug.cgi?id=1788206)
- Emscripten GL modes (FULL_ES3 emulates mapped buffers, client arrays):
  https://emscripten.org/docs/porting/multimedia_and_graphics/OpenGL-support.html
- virglrenderer (read from a GitHub fork of upstream main, since
  gitlab.freedesktop.org blocks fetches): `src/vrend/vrend_renderer.c`
  (`FEAT()` table, `vrend_fill_caps_glsl_version()`), `vtest/vtest_protocol.h`
  (`VCMD_*`, protocol versions).
- Mesa `src/gallium/winsys/virgl/vtest/virgl_vtest_winsys.c`,
  `virgl_vtest_flush_frontbuffer()` (present = transfer_get + displaytarget).
- Boxedwine: guest OpenGL calls are implemented by the emulator and passed to
  SDL's GL, which in the Emscripten build is GLES3 on WebGL2 (`-sFULL_ES3`,
  `-sOFFSCREENCANVAS_SUPPORT`, `-sPROXY_TO_PTHREAD`, project/emscripten/makefile):
  API-level passthrough, not a command stream: https://github.com/danoon2/Boxedwine
- wasm-vm (BLamy): virglrenderer 1.3.0's shader translator built to wasm,
  GLSL ES 3.00 out, checked against WebGL2: https://github.com/BLamy/wasm-vm/pull/403 ;
  its plans for virgl→WebGPU (TGSI→WGSL, pipeline cache) are not built yet:
  tasks/epic-6-transcendence/E6-T10, E6-T12 in that repo.
- gl4es (desktop GL 2.1 on GLES2, has an Emscripten target):
  https://github.com/ptitSeb/gl4es — an alternative only for an API-passthrough
  design (Boxedwine's), not for a command stream.
- ANGLE on WebGPU (Dawn) and WebGPU compatibility mode (Khronos 2025/2026):
  https://www.khronos.org/assets/uploads/developers/presentations/WebGL+WebGPU_SIGGRAPH_2025.pdf
- wgpu (WebGPU-shaped API over Vulkan/Metal/D3D12/GL; WebGL2/WebGPU in wasm):
  https://github.com/gfx-rs/wgpu
- v86 (VGA/SVGA only), qemu-wasm, WebVM/CheerpX: no guest GPU path in their
  docs: https://github.com/copy/v86 , https://github.com/ktock/qemu-wasm ,
  https://webvm.io/

## End to end in the real machine (branch gpu, 2026-10-10)

Debian `daily` disk, headless Chrome for Testing on the M4 Max (ANGLE Metal), vrend in a Worker, stage A
(Mesa's own present: a readback per frame into the window's wl_shm buffer). One run each unless said.

What had to be fixed before anything was drawn (each found in a browser hold, `build/h*.sh`):
1. **No transfers inside command buffers.** vrend's caps said `VIRGL_CAP_TRANSFER` and
   `VIRGL_CAP_COPY_TRANSFER`, so Mesa sent uploads as `VIRGL_CCMD_TRANSFER3D` inside SUBMITs, which vrend
   serves through the resource's own iovec, and our resources have none (their backing is in the machine's
   memory, another wasm memory): "Illegal resource", and every later draw refused. The bits are cleared in
   `rvg_caps()`: Mesa then uses vtest's TRANSFER_PUT2/GET2, i.e. our ring records.
2. **vrend's stack.** Emscripten's default stack is 64 KiB; vrend's shader translator keeps its whole state
   on the stack and ran over the module's data unseen: a vertex shader came out asking for
   `GL_OES_shader_io_blocks` (a geometry shader "present" in a key read from trampled memory), then malloc's
   heap broke. 4 MiB now, with Emscripten's overflow check.
3. **vrend's log.** Its stdio logger trapped ("null function" in `__fwritex`) on the long message of the
   failed shader, and a trap leaves the module unusable; messages go through `emscripten_err` now.
4. **Texture swizzle.** GL ES 3.0 has `GL_TEXTURE_SWIZZLE_*`, WebGL2 does not, and vrend relies on it on
   GL ES hosts (A8 kept as R8 and read as 0,0,0,R; X8 formats' alpha = 1; the guest's own swizzles). Qt
   Quick's glyphs came out as solid boxes. Now every sampler view whose swizzle is not the identity puts
   it into the shader key, and the shader wraps `texture()` in `rv_swizzle()` (patch in
   `gpu/vrend/virglrenderer-wasm.diff`; texelFetch already had vrend's own "lower swizzle" path).
Also: failing records are reported with their words and traps with vrend's function names
(`--profiling-funcs`), which is how 2 and 3 were found.

What the guest sees (eglinfo -B, Wayland): `virgl (WebKit WebGL)`, **OpenGL ES 3.0** (GLSL ES 3.00) and
**OpenGL 2.1 compatibility** (GLSL 1.20), no core profile. vrend: "GLSL feature level 130".

| test (800x600 unless said) | GPU (virgl -> vrend -> WebGL2) | software (llvmpipe, or Qt's software renderer) |
| --- | --- | --- |
| glmark2-es2-wayland, 6 scenes (build, texture, phong, bump, effect2d, refract) | score **186** (214, 221, 233, 131, 172, 152 fps) | score **5** (1, 7, 3, 14, 11, 1 fps) |
| es2gears_wayland 300x300 | 55-60 frames/s (wlserver presents), drawn right | drawn (h1), rate not recorded |
| Qt Quick, 200 rotating rectangles + text (`qml spin.qml`) | **41-60 fps** (QRhi OpenGL on "virgl", GL 2.1 context) | **11-15 fps** (`QT_QUICK_BACKEND=software`) |
| Chromium 145, local page (120 CSS-animated boxes + a 2D canvas) | 1.8-3.3 fps; WebGL in the page works ("ANGLE (Mesa, virgl (WebKit WebGL), OpenGL ES 3.0)") | 0.1-1.7 fps, no WebGL |

Readback cost (stage A): vrend's synchronous `glReadPixels` is about 1.35 ms a frame for a 300x300 window
and ~1.6 ms for 800x600 (exec stats: `getMs` / `gets`); then two guest copies and the page's upload, as for
any wl_shm window.

Chromium: with `--use-gl=angle --use-angle=gles-egl` its GPU thread starts ANGLE on Mesa's GL ES over
virgl (WebGL works), but compositing stays in software: Ozone/Wayland wants a DRM render node for its
buffers ("drmGetDevices2() has not found any devices", "No suitable EGL configs found"), and only 17 ring
records were executed in 90 s. GPU compositing in Chromium therefore needs the same thing as stage B:
an emulated render node (rv-virtgpu, PROTOCOL.md). The fps difference above is from one run each and
not explained (the software run slowed down over time); the flags are kept as the default when the GPU
is there because the page renders right and WebGL works, and `RVGPU_CHROMIUM=0` turns them off.

Stage B (zero copy) is not reachable with stock Mesa's GL: its vtest winsys presents through wl_shm only;
`zwp_linux_dmabuf_v1` buffers come from Mesa's DRM paths (virtio_gpu_dri, venus WSI), which need the
emulated render node. wlserver's dmabuf.js and display.js's format-2 slots are ready and unused.

Default (tools/x/desktop): every `desktop wayland` program (and what the page's panel starts, which goes
through it) uses the GPU when an executor answered; `RVGPU=0` software, `RVGPU=1` wait up to 10 s and say
so; Blender and `RVGPU_SOFTWARE`'s program names stay on llvmpipe (Blender 4.3 needs GL 4.3 core and exits
on GL 2.1). gpuserver writes `rvgpu-0.none` when no executor answered in 10 s, so later programs do not
wait; the page's `?gpu=off` starts no executor.
