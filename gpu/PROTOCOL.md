# gpu: guest programs' GL (and later Vulkan) drawn by the page

The agreed document between gpu-guest (Mesa in the guest, the socket server
in the machine, this branch) and gpu-host (the executor in the page, branch
gpu-host, whose `gpu/FEEDBACK.md` this answers). Version 1, 2026-10-06.
Changes are made here; gpu-host's objections go in its FEEDBACK.md.

## For gpu-host's e2e run (2026-10-06 ~15:05)

`build/e2e.sh` in gpu-host runs `debian "RVGPU=1 desktop wayland
/usr/lib/rvgpu/bin/$P"`. Inside `debian` the GPU server's directory is
**/run/rvgpu** (tools/debian-enter binds the initramfs' /usr/lib/rvgpu
there, as for /run/wlserver); /usr/lib/rvgpu does not exist in the Debian
root, so the program would not be found. Use `/run/rvgpu/bin/$P`, or just
`$P`: since 2026-10-06 ~15:10 `RVGPU=1 desktop wayland` puts /run/rvgpu/bin
on the PATH. Result of the first run (gpu-host build/e2e.out, 14:57): as
feared, the program was not found (the job ended at once, no client); but
gpuserver started and the real executor (vrend in a Worker, ANGLE Metal)
found it and wrote 1408 bytes of caps: the start-up handshake with the
real executor works. Next run: same, with the path fixed. The rest
of the guest side in that tree (stage.sh's /usr/lib/rvgpu with the demos,
debian-enter, desktop's RVGPU=1) is what was tested here under node-machine.
Logs to read afterwards: /root/rvgpu.log (gpuserver; `RVGPU_STATS=1` adds a
line every 5 s), /root/wlserver.log (`WLSERVER_STATS=1`), and the program's
own output; `desktop` prints "no GPU (no executor answered ...)" and falls
back to software when the executor has not written its caps within 10 s
(`RVGPU_WAIT=N` tenths of a second to wait longer).

## The decision in one picture

    guest program under rv (glxgears, es2gears, Qt Quick, ...)
      stock Debian Mesa 25.0.7: libgallium's virgl driver with the vtest
      winsys ("virpipe"); no DRM device, no kernel driver, no Mesa change
        | Unix socket $XDG_RUNTIME_DIR/rvgpu-0: vtest protocol 4 (VCMD_*),
        | command buffers inline, resource backing in files the server
        | makes, passed with SCM_RIGHTS and mapped by both sides
    gpuserver: a JS process of the machine (the hostnode way, like
    wlserver); speaks vtest's server side; never decodes virgl commands
        | ring in its memory (below): one record per vtest command that
        | needs the GPU; resource backing = linear-memory offsets;
        | doorbell = Atomics.notify on the ring; completions = a word the
        | executor writes (and Atomics.notify)
    executor: page code (a Worker with an OffscreenCanvas, or display.js's
    own context): virglrenderer's vrend built to wasm on WebGL2   (gpu-host)
        | present: stage A through wl_shm (Mesa's own software display path),
        | stage B a GPU resource in the window's slot (format 2)
    display.js composites

This is what gpu-host proposed (FEEDBACK.md), with three corrections from
measurements in this branch: the guest sees GL 2.1 + GLES 3.0, not GL 3.0, on a
GLES 3.0 host (below); stage A cannot be zero-copy with stock Mesa; the
zero-copy present (stage B) comes in through `zwp_linux_dmabuf_v1`, which
stock Mesa speaks, not through a Wayland protocol of our own, which needs a
Mesa fork.

## Verified (build VM, 2026-10-06, `gpu/native-vtest.sh`)

The guest side needs no kernel driver and no change to Mesa. Ubuntu's
`virgl_test_server` (virglrenderer 1.0.0, arm64, the VM's Mesa llvmpipe
behind EGL surfaceless) on one end of a Unix socket, Debian's riscv64 programs
under rv (interpreter, native build) on the other, with
`GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1 VTEST_SOCKET_NAME=<socket>`:

| test | result |
| --- | --- |
| `eglinfo` (surfaceless), host desktop GL | `virgl (LLVMPIPE (LLVM 20.1.2, 128 bits))`, OpenGL 4.3 core, ES 3.2 |
| same, host GLES 3.2 (`--use-gles`) | GL 4.3 core and compat, ES 3.2 |
| host GLES forced to 3.1 (`MESA_GLES_VERSION_OVERRIDE=3.1` on the server) | GL 4.2 core, ES 3.1 |
| host GLES forced to 3.0 (WebGL2's level) | **no core profile; GL 2.1 compatibility (GLSL 1.20), ES 3.0 (GLSL ES 3.00)** |
| `peglgears` (pbuffer), host GLES 3.0 | 5472 frames in 5.0 s = 1094 fps |
| `peglgears`, guest llvmpipe instead (`GALLIUM_DRIVER=llvmpipe`) | 2 frames in 22.7 s = 0.088 fps |
| `vulkaninfo --summary`, venus (`VN_DEBUG=vtest`, server `--venus`) | `Virtio-GPU Venus (llvmpipe ...)`, Vulkan 1.2 device, driver venus 25.0.7 |

Caveats: the native rv is the interpreter (the browser's JIT is much faster,
so the llvmpipe number is not what the browser would see, only the order of
magnitude of the gap); `MESA_GLES_VERSION_OVERRIDE=3.0` lowers llvmpipe's
version but not all of its extensions, so a real WebGL2 host can only come
out at or below "GL 2.1 + ES 3.0". The same 2.1 is what UTM guests report on
ANGLE's Metal backend, which is ES 3.0 (see sources).

What the guest therefore gets on WebGL2: OpenGL ES 3.0 and desktop GL 2.1
(compatibility). Qt Quick's OpenGL backend needs GL 2.1 or ES 2.0/3.0;
what GTK4, Chromium's GPU process and SDL games need is to be checked
against that (not done). GL 3.2+ core is not reachable on a WebGL2 host:
virglrenderer needs geometry shaders for it (GLES 3.2), and WebGL2 and WebGPU
have none.

What Debian riscv64 ships (trixie, read from the installed root and from
packages.debian.org file lists): `libgl1-mesa-dri` with `virtio_gpu_dri.so`,
`zink_dri.so`, `swrast_dri.so`, `kms_swrast_dri.so`; `libgallium-25.0.7` has
virgl with the vtest winsys (strings `virpipe`, `VTEST_SOCKET_NAME`,
`/tmp/.virgl_test`); `mesa-vulkan-drivers` (not installed in
debian-root-kde) has `libvulkan_virtio.so` (venus, with its vtest backend),
`libvulkan_gfxstream.so`, `libvulkan_lvp.so`; `mesa-utils-bin` (not installed)
has glxgears, eglinfo, es2gears_wayland, eglgears_wayland, peglgears;
`vulkan-tools` has vkcube, vulkaninfo. `virgl-server` is not built for
riscv64 (not needed: the server is ours).

## The socket (guest ↔ gpuserver)

- Path `$XDG_RUNTIME_DIR/rvgpu-0`; clients find it by `VTEST_SOCKET_NAME`,
  which `desktop wayland` exports together with `GALLIUM_DRIVER=virpipe`
  when a gpuserver with a live executor runs (only then: a virpipe client
  without a server aborts in `virgl_block_read`). `LIBGL_ALWAYS_SOFTWARE=1`
  stays set: it is what makes EGL and GLX take the software path that
  virpipe lives on. Without the variables programs get llvmpipe as today.
- The protocol is vtest as in virglrenderer's `vtest/vtest_protocol.h`
  (version 4) and Mesa's `virgl_vtest_socket.c` / `vn_renderer_vtest.c`:
  little-endian dwords, header `{length_dw, cmd_id}`, then `length_dw`
  dwords. gpuserver answers version 3 (resource ids made by the server,
  backing for every resource with a size) and refuses 2 and older: Mesa's
  virgl asks for 3 and drops to 2 after `GET_CAPS2` when the caps'
  `host_feature_check_version` (offset 556 of `struct virgl_caps_v2`) is
  below 23, and version 2's resource handles are the client's own, which
  collide between clients. **The executor's caps must say 23 or more**:
  virglrenderer main (1.3.0) does (`vrend_renderer.c`); Ubuntu's 1.0.0 says 19
  (measured), so caps from an old virglrenderer make every client fail. It
  implements, for the
  GL client: `CREATE_RENDERER`, `PING_PROTOCOL_VERSION`, `PROTOCOL_VERSION`,
  `GET_CAPS2`/`GET_CAPS`, `RESOURCE_CREATE2`, `RESOURCE_UNREF`,
  `TRANSFER_GET2`, `TRANSFER_PUT2`, `SUBMIT_CMD`, `RESOURCE_BUSY_WAIT`,
  `RESOURCE_CREATE_BLOB` (persistent/coherent buffer maps; the blob is
  created by a `PIPE_RESOURCE_CREATE` inside a preceding `SUBMIT_CMD`).
  Venus' commands (`GET_PARAM`, `GET_CAPSET`, `CONTEXT_INIT`, `SYNC_*`,
  `SUBMIT_CMD2`) are stage C.
- Semantics kept from virglrenderer's own server (`vtest_renderer.c`): one
  renderer, one context per connection, resource ids global; a resource's
  backing is a shared-memory file of its size, sent after the
  `RESOURCE_CREATE2` reply; `TRANSFER_GET2`/`PUT2` move data between the
  GPU resource and the backing at `offset`, with the strides virglrenderer
  uses when the client gives none; `RESOURCE_BUSY_WAIT` answers whether the
  work submitted so far is done (with flag WAIT: after it is).
- Backing files are made the way wlserver's pools need them: a file in a
  ramfs (`$XDG_RUNTIME_DIR`), unlinked, `ftruncate` up from size 0 once,
  never grown, so the kernel gives contiguous pages and `mmap(MAP_SHARED)`
  returns their linear-memory offset; the client's rv maps the same pages
  (`map_host_shared()`, which must take its direct path, never
  `live_attach()`: checked with RV_STRACE in the prototype). A size that cannot
  be had in one piece fails the resource (the client sees `RESOURCE_CREATE2`
  fail and Mesa reports out of memory) rather than silently copying.

## The ring (gpuserver → executor), version 2

gpuserver allocates one block in its memory and announces it on
`BroadcastChannel("rv-gpu-ID")` (ID: the page load's, `rvtab=ID` on the kernel's command line; `rv-gpu` where there is none) at start and once a second:
`{ type: "gpu", version: 2, pid, ring, ringSize, caps, capsSize, exports,
exportsCap, socket }` (addresses are linear-memory offsets as Numbers), and
at once whenever someone posts `{ type: "gpu?" }` on that channel (the window
server does at its start; the executor may too; consumers must ignore
messages of other types). The page hands the machine's `WebAssembly.Memory`
to the executor.

Header (2048 bytes at `ring`; 32-bit little-endian words; "exec" marks words
the executor writes, all others are gpuserver's):

| offset | field | meaning |
| --- | --- | --- |
| 0 | magic | `0x50475652` ("RVGP") |
| 4 | version | 2 |
| 8 | pid | gpuserver's process id |
| 12 | heartbeat | +1 every gpuserver turn |
| 16 | data_size | bytes of the data area (a power of two, 4 MiB) |
| 20 | data_off | where the data area starts, from `ring` (2048) |
| 24 | head | bytes written into the data area, counting up (mod 2^32) |
| 28 | doorbell | +1 after every batch of records; `Atomics.notify` on it |
| 32 | submitted | serial of the newest record that needs completion |
| 40, 44 | exports_lo, exports_hi | where the export table is (Presenting, stage B) |
| 48 | exports_cap | its entries |
| 64 | tail | exec: bytes consumed (the executor may reuse nothing past it) |
| 68 | done | exec: newest serial such that it and every record before it are done; `Atomics.notify` on it after ANY completion (also one that only moved a context's word) |
| 72 | exec_heartbeat | exec: +1 every executor turn; still for 2 s = executor gone |
| 76 | exec_state | exec: 0 not yet, 1 ready (caps written), 2 context lost, 3 failed |
| 80 | exec_error | exec: a code when state is 2 or 3 |
| 84 | caps_len | exec: bytes of caps written at `caps` (virgl caps v2, as `virgl_renderer_fill_caps(2, ...)`) |
| 1024 + 4·ctx | ctx_done[ctx] | exec: for context `ctx` (1..255), the newest serial of its records such that they all are done |

Records in the data area, **16-byte aligned**, never wrapping: a record
takes 16 bytes of header plus its payload rounded up to 16, so what is left
before the end is always 0 or at least 16 bytes, and a record that does not
fit is preceded by a PAD record filling the rest (its `bytes` = the rest
minus 16). Each record: `{u32 op, u32 ctx, u32 serial, u32 bytes}` +
`bytes` of payload. `serial` counts up per record from 1 (mod 2^32;
compare with `(a - b) | 0 >= 0`); records with "completion" below raise
`submitted`. Contexts are numbered 1..255 and numbers are used again only
after `done` has passed the old context's CTX_DESTROY.

| op | name | payload (u32 unless said) | completion |
| --- | --- | --- | --- |
| 0 | PAD | — | — |
| 1 | CTX_CREATE | name (UTF-8, NUL-terminated: the process name the client sent) | no |
| 2 | CTX_DESTROY | — (the connection closed; its resources are unreferenced before) | no |
| 3 | RES_CREATE | res, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags, size, addr_lo, addr_hi (backing; 0 when size is 0) | no |
| 4 | RES_UNREF | res (gpuserver unmaps the backing only after `done` passed this record) | no |
| 5 | SUBMIT | the command buffer's dwords, as the client sent them | yes |
| 6 | TRANSFER_PUT | res, level, x, y, z, w, h, d, offset, data_size (data is in the backing) | yes |
| 7 | TRANSFER_GET | res, level, x, y, z, w, h, d, offset, data_size | yes (the data is in the backing when the context's `ctx_done` passes it) |
| 8 | BLOB_CREATE | res, blob_id, size, flags, addr_lo, addr_hi, type (memory the GPU buffer of `blob_id` is mirrored in) | no |

(Version 1 had a PRESENT record; it is gone: a GPU window's present is
wlserver's slot, Presenting, stage B.)

Rules:
- gpuserver reads a client's bytes into a block of its own memory and moves
  a `SUBMIT_CMD`'s dwords from there into the ring with one `copyWithin`
  (no copy through JavaScript arrays). A command buffer bigger than a
  quarter of the data area is refused (Mesa's virgl batches are far smaller).
- Completion is in order within a context: the executor raises
  `ctx_done[ctx]` to a record's serial only when that record's work and the
  context's records before it are finished, i.e. for TRANSFER_GET when the
  bytes are in the backing (readPixels into a PIXEL_PACK buffer, a fence,
  polled with timeout 0, then getBufferSubData into a view over the shared
  memory: FEEDBACK.md, constraint 3). `done` is the same over all records
  (for gpuserver's unmapping and context reuse). One client's slow readback
  therefore holds back only its own replies (FEEDBACK.md, point 8).
  gpuserver answers `RESOURCE_BUSY_WAIT` from `ctx_done[ctx]` vs. the
  client's newest completion serial; with WAIT it holds the reply until then.
- Latency: gpuserver sleeps in `epoll_pwait` (≤ 8 ms) only while no client
  waits for the executor; while one does, it waits on `done` with
  `Atomics.wait` in slices of 1 ms between `epoll_pwait(…, 0)` calls, so a
  glXSwapBuffers costs one executor round trip and not an 8 ms turn. The
  executor waits on `doorbell` the same way (a Worker blocks in
  `Atomics.wait`; the main thread uses `Atomics.waitAsync`).
- Executor gone (heartbeat still 2 s) or state 2/3: gpuserver fails every
  waiting client's reply by closing its socket (Mesa aborts that program:
  the GPU program dies, the tab does not), refuses new connections, and
  removes `rvgpu-0.ready`, so `desktop wayland` stops pointing programs here.
  (The ready file holds `{"pid", "ring", "exports", "exportsCap"}` as JSON:
  the window server reads it when it has not heard an announcement yet, since
  a BroadcastChannel message can be missed by whoever starts later.)
  Context loss recovery is not attempted; once it has no clients left,
  gpuserver removes its socket and exits, so the next `RVGPU=1 desktop
  wayland` starts a fresh one that a new executor (page reloaded) finds.
  Seen under node-machine (stub stopped after 30 s): es2gears_wayland ended
  with Mesa's abort (status 134, "lost connection to rendering server"), the
  window server and the machine went on, gpuserver exited, and the next
  `RVGPU=1` program fell back to llvmpipe with "desktop: no GPU".
- Caps: gpuserver answers `GET_CAPS2` from the caps the executor wrote at
  start (`caps_len` bytes; the reply's length word is `caps_len + 1`, in
  bytes, not dwords, as in `vtest_send_caps2`) and `GET_CAPS` (v1) from their
  first `sizeof(struct virgl_caps_v1)` (308) bytes. Connections made before
  `exec_state` is 1 wait for it (up to 10 s, then are closed).
- Strides (FEEDBACK.md, point 2): vtest's `TRANSFER_GET2`/`PUT2` carry no
  stride, and virglrenderer's server passes stride 0, i.e. the level's width
  times the bytes per block; Mesa's vtest winsys lays the backing out with
  the same natural strides (`virgl_vtest_flush_frontbuffer()` computes
  `offset` from `util_format_get_stride(format, width)`). gpuserver knows
  nothing more than the client sent, so there is no stride to add: the
  executor uses the natural ones.
- `flags` in RES_CREATE (point 4): vtest's `RESOURCE_CREATE2` has no flags
  field, so under vtest it is always 0 (never `VIRGL_RESOURCE_Y_0_TOP`):
  window buffers come bottom row first, as gpu-host expects. The word stays
  for the DRM path (rv-virtgpu), where Mesa passes flags.

## Answers to FEEDBACK.md (gpu-host 71cb2de)

1 PAD: records are 16-byte aligned (above), so a PAD always fits; please
make the executor's tail step `16 + ((bytes + 15) & ~15)`. 2 strides and
4 orientation: above (natural strides; `flags` is 0 under vtest). 3 BGRA
readbacks through a swizzle draw: agreed. 5 `Atomics.waitAsync` on the main
thread: agreed (the Rules say so now). 6 BLOB_CREATE: agreed — on a WebGL2
host vrend has no buffer storage, so Mesa should not ask; gpuserver keeps
the record for completeness. 7 caps: agreed, with the one requirement found
here: `host_feature_check_version` ≥ 23 (the vtest section). 8 per-context
completion: done (`ctx_done[]`, ring version 2), and PRESENT is gone from the
ring.

## Presenting

Stage A (now; what stock Mesa does): `eglSwapBuffers`/`glXSwapBuffers` on
virpipe is `virgl_vtest_flush_frontbuffer()`: `TRANSFER_GET2` of the drawable
(or the damaged box) into the backing, `RESOURCE_BUSY_WAIT`, a copy into
the software winsys's display target, and that winsys' own present (EGL on
Wayland: a copy into a `wl_shm` buffer and `wl_surface.commit`; GLX: XPutImage).
So per frame: one GPU readback (executor), two copies in the guest, one
upload by display.js. Correct and simple; it is the baseline to measure
stage B against. Back buffers on Wayland are what EGL's configs say
(ARGB8888/XRGB8888 visuals = B8G8R8A8/B8G8R8X8): the readback must produce
BGRA byte order for those; vrend's format table does this.

Stage B (zero copy): the window's buffer is a GPU resource. wlserver gets it
through `zwp_linux_dmabuf_v1` (version 3 or 4: what Mesa's Vulkan WSI and
EGL's DRM path bind): `create_params`, `add(fd, plane 0, offset, stride,
modifier LINEAR)`, `create_immed`. The fd is one of gpuserver's backing
files; gpuserver publishes an **export table** in its memory (address in
the ring header and the announcement: a generation word, +1 at every change;
the number of entries; 8 bytes spare; then per entry 32 bytes `{dev (low 32
bits), ino_lo, ino_hi, res, ctx, 3 spare}`, res 0 = free, written last);
wlserver `fstat`s the fd, finds the inode, and writes the slot as format 2
(buffer_lo = res, buffer_hi = gpuserver's pid, which is what gpu-exec.js's
`rvGpu.texture(pid, res)` checks; resource numbers are gpuserver's, global
over contexts, so the context is not needed to find the texture — it is in
the export table for later per-context ordering), so the page draws the
executor's texture of that resource. **For gpu-host**: display.js (3f1f651)
names buffer_hi `ctx` and passes it on as `texture(ctx, res)`; it is the
pid, which is what texture() expects. Also gpu-exec.js accepts only
`version === 1` announcements (line ~982 at 3f1f651); they are version 2 now. A dmabuf fd that is not a GPU resource (any mappable file) is
drawn like a wl_shm buffer at offset/stride, the readback written into it by
the executor on a TRANSFER_GET: the fallback, and how stage B can be tried
before zero copy works. Frame callbacks and `wl_buffer.release` stay as for
wl_shm (uploaded/shown ids), now meaning "the page drew that texture".
Ordering: when wlserver writes such a slot it also writes gpuserver's
`submitted` word of that moment into the slot (`gpu_serial`, slot offset
244; main took 232..240 for desktop, shell flags and tile), and the page
draws the texture only once `done` has passed it — the client's rendering
of that frame was submitted before its `wl_surface.commit`, so it is done by
then. (Conservative: it waits for other clients' work too. Per-context would
need the dmabuf's context, which the export table could carry later.)
Known gap: the client's last SUBMIT_CMD travels on the GPU server's socket and
its commit on the window server's; nothing orders the two, so wlserver may
read `submitted` before gpuserver has read the frame's commands, and the page
could draw the texture one frame early (old contents, never torn memory).
Mesa's venus WSI and an emulated render node would need explicit sync
(`wp_linux_drm_syncobj_v1`, which Mesa's WSI binds when offered) to close it;
measured as a problem or not once a real client runs.

Slot formats 3 and 4 (new, from dmabuf buffers that are plain files in
`ABGR8888`/`XBGR8888`, i.e. bytes R, G, B, A): the page must draw them
without the .bgra swizzle (4 also opaque). **For gpu-host / display.js**:
handle 3 and 4; until then such windows show red and blue swapped.

Done in this branch (wlserver/dmabuf.js, only with `WLSERVER_DMABUF=1`,
which `desktop wayland` sets with `RVGPU=1`): `zwp_linux_dmabuf_v1` version 3
(format + LINEAR modifier events for AR24, XR24, AB24, XB24),
`zwp_linux_buffer_params_v1` add / create / create_immed, one plane, LINEAR
(or INVALID) modifier, no flags; the export-table lookup; slot format 2 with
`gpu_serial`; plain files mapped (or read per commit) like wl_shm pools.
Mesa's software EGL ignores the global (it binds wl_shm only), so programs
that ran before run the same. Tested under node-machine with
`gpu/tools/dmabuf-test.py` (a hand-written Wayland + vtest client in Python):
a memfd buffer drawn and redrawn (6 presents, 6 releases), and a GPU
resource's backing fd taken as a dmabuf → slot format 2, "GPU resource 1 of
context 1"; es2gears_wayland and weston-simple-shm unchanged.

Which guest programs produce such buffers with stock Mesa: Vulkan through
venus (WSI exports the blob's fd: `vtest_bo_export_dma_buf()` dups the
backing fd gpuserver sent; Mesa's Wayland WSI requires
`zwp_linux_dmabuf_v1` ≥ 3 for a non-software device); GL through zink on
venus (kopper uses the same WSI); GL through the virgl DRM winsys
(`virtio_gpu_dri.so`), which needs a DRM device: **rv-virtgpu**, an emulated
virtio-gpu render node in rv (open of `/dev/dri/renderD128`, the
`DRM_IOCTL_VIRTGPU_*`, PRIME and GEM ioctls, `mmap` of the fd, `fstat`
rdev 226:128, the few sysfs files libdrm's `drmGetDevice2()` reads) that
rv turns into vtest messages to gpuserver. That is the guest side's stage B
work item; it needs no kernel change and makes GBM, EGL's dmabuf path and
Xwayland's glamor possible too. A Wayland protocol of our own
(`rv_gpu_buffer_v1`, FEEDBACK.md) or a vtest present command would need a
patched Mesa (EGL's Wayland platform or the vtest winsys): not proposed.

## Prototype status (2026-10-06)

Built (this branch): `gpu/server/` (gpuserver: vtest 3 server side for the
GL client, backing files, the ring, completion, caps, busy waits, the ready
file, the export table, small backings kept for reuse because Mesa makes
every fence an 8-byte resource, one or more per frame), `tools/x/desktop`
(`RVGPU=1 desktop wayland PROGRAM` starts it and points the program at it
once an executor answered), `tools/debian-enter` (`/run/rvgpu`),
`gpu/tools/gpu-stub.js` (a stand-in executor for tools/node-machine.js:
recorded caps, completes every record at once, fills each TRANSFER_GET box
with a colour that changes per frame), `gpu/tools/nm-run.sh`,
`gpu/native-vtest.sh`, `gpu/tools/vtest-caps.py` and
`gpu/tools/caps-gles3.0.bin`.

Seen under node-machine (the machine's real kernel and rv; Node's V8, not
Chrome; no drawing — the stub):
- `eglinfo` through gpuserver: `virgl (LLVMPIPE ...)`, GL 2.1 compat,
  ES 3.0, i.e. the GLES 3.0 caps, the client's whole start-up (create
  renderer, version, caps, resources with their backing fds, 8 command
  buffers).
- `RVGPU=1 desktop wayland es2gears_wayland` under wlserver with
  tools/wl-dump.js chained: a 300x300 window whose every frame is the stub's
  colour (build/shots-gpu): client → socket → gpuserver → ring → stub writes
  the backing → the client's mapping sees it (so rv's `map_host_shared()` took
  its direct path: a `live_attach()` copy would have stayed black) → Mesa
  copies it into a wl_shm buffer → wlserver → table. 10 to 40 presents a
  second over runs (noisy: the Mac was shared with other agents' work), one
  `RESOURCE_BUSY_WAIT` per frame answered in 4 to 14 ms with the stub polling
  every 1 ms from Node's main thread. The same program on llvmpipe in the
  guest (no RVGPU) showed no frame in 60 s.

Since then (2026-10-06 afternoon): the dmabuf path in wlserver (stage B's
window-server half, above), tried under node-machine with
`gpu/tools/dmabuf-test.py`; gpu-host's vrend (virglrenderer 1.1.1, which
reports host_feature_check_version 23) builds to wasm and its executor reads
ring version 2 (gpu-host 3f1f651). `build/e2e-hold.sh` (untracked) is the
first browser hold of both halves together (stage this tree, gpu-host's
gpu-exec.js/display.js/vrend over it, es2gears_wayland): waiting for the
browser.

Not done: venus' commands; executor loss recovery; rv-virtgpu (needs the
owner's OK); anything in the browser.

## rv-virtgpu: the emulated render node (branch virtgpu, 2026-10-10)

`rv/virtgpu.c`. With `RV_VIRTGPU=SOCKET` in the environment (`desktop` sets it
for every program with `RVGPU_DRM=1`), rv gives guest programs a virtio-gpu
device the way QEMU's virtio-gpu-pci looks to a Linux guest, and carries out
its DRM interface (Linux 7.0's `include/uapi/drm/virtgpu_drm.h`, `drm.h`) as
vtest 3 commands on a connection to the GPU server: gpuserver in the machine,
or virglrenderer's `virgl_test_server` natively. Nothing changes in gpuserver,
the ring or the executors; to them it is one more vtest client per open.

- **Files.** `/dev/dri/renderD128` (a character device 226:128 to stat),
  `/sys/dev/char/226:128` → `/sys/devices/pci0000:00/0000:00:02.0/virtio0/drm/renderD128`
  with `uevent`, `dev`, `device`, `subsystem` links, the PCI device's
  `vendor`/`device`/`subsystem_*` (1af4:1050, as QEMU's), `virtio0`'s
  (1af4:0010, which Mesa's loader reads), `/sys/class/drm`. They are real files
  in `/tmp/.rv-virtgpu-1-UID` (made by the first process that needs them),
  and rv takes those paths there (`rv_virtgpu_path()`), so `opendir`,
  `readlink` and `realpath` (libdrm's `drmGetDevices2`, `drmGetDevice2`,
  `drmParseSubsystemType`, Mesa's loader) work as on Linux. No card0: the
  device has no KMS.
- **Opening** the device connects to the server (`CREATE_RENDERER` with the
  program's name, `PROTOCOL_VERSION` 3); the program's descriptor is that
  socket. Its `fstat` says the device. A connection made by another process
  and passed on (a sandbox broker) is taken as the device too.
- **ioctls** (only `'d'` ones; per open, one at a time):

  | ioctl | done as |
  | --- | --- |
  | `VERSION` | "virtio_gpu" **0.1.0** when the server makes fences as files (gpuserver: its ready file says `"features": ["fence"]`), else 0.0.0 (Mesa then makes its fences from 8-byte resources and waits on them) |
  | `GET_CAP` | PRIME import/export, TIMESTAMP_MONOTONIC; SYNCOBJ 0; KMS caps: EOPNOTSUPP |
  | other DRM core ioctls | EACCES, as on a render node (SYNCOBJ_*: EOPNOTSUPP) |
  | `VIRTGPU_GETPARAM` | 3D 1, CAPSET_QUERY_FIX 1, RESOURCE_BLOB 0, HOST_VISIBLE 0, CROSS_DEVICE 0, CONTEXT_INIT 1, capsets VIRGL and VIRGL2 |
  | `GET_CAPS` | `GET_CAPS2` once, kept (capset 1: its first 308 bytes); other capsets EINVAL (Mesa's native-context probe) |
  | `CONTEXT_INIT` | capset VIRGL/VIRGL2 accepted; EEXIST the second time |
  | `RESOURCE_CREATE` | `RESOURCE_CREATE2` (the server's id is the `res_handle` command streams name), backing fd received with it, GEM handle = lowest free |
  | `RESOURCE_INFO`, `MAP` | from the handle's state; `MAP`'s offset is `handle << 32`, and an `mmap` of the device at it maps the backing file (`map_host_shared()`: the very pages gpuserver and the executor see) |
  | `TRANSFER_TO_HOST` / `FROM_HOST` | `TRANSFER_PUT2` / `GET2`; `data_size` counted as Mesa's vtest winsys counts it (natural strides, the format's block size: `rv/virtgpu_formats.h`, generated by `gpu/tools/gen-virgl-formats.py`), the rest of the backing for formats not sized |
  | `WAIT` | `RESOURCE_BUSY_WAIT` (NOWAIT: EBUSY while busy) |
  | `EXECBUFFER` | `SUBMIT_CMD`; ring 0 only, no syncobjs. Out-fence: `VCMD_RV_FENCE` (0x10001, gpuserver's own): an eventfd it writes once the context's `ctx_done` passes the client's last record (readable = signalled, as a sync_file); in-fence: polled before the submit. Without the feature: waits, then a signalled eventfd. `SYNC_IOC_MERGE` / `FILE_INFO` on these fences are answered by rv |
  | `GEM_CLOSE` | `RESOURCE_UNREF` |
  | `PRIME_HANDLE_TO_FD` / `FD_TO_HANDLE` | a copy of the backing file's descriptor / found by the file's identity among this open's buffers |

- **Presenting** (stage B): Mesa's EGL on Wayland takes its DRM path only if
  the compositor names its device. wlserver does with
  `WLSERVER_DMABUF_DEVICE=226:128` (`desktop` sets it with `RVGPU_DRM=1`):
  `zwp_linux_dmabuf_v1` version 4, feedback with that `main_device` and one
  tranche of the four formats with LINEAR and INVALID (Mesa's virgl has no
  explicit modifiers and needs the implicit one offered). Window buffers then
  are GPU resources handed over as dmabufs, i.e. gpuserver's backing files,
  which the export table turns into slot format 2.
- **Not yet:** blobs and host-visible memory, venus' capset (needs gpuserver's
  `CONTEXT_INIT`/`SUBMIT_CMD2`, T5), syncobjs, importing a buffer another
  process made (needs the export table's reverse: dmabuf → resource, plus
  attaching the resource to this context in the executor).

Tried: natively (`gpu/tools/virtgpu-native.sh`: native rv, `virgl_test_server`
1.1.0 on llvmpipe in the trixie chroot) `eglinfo` (surfaceless, device
platforms: `virgl (LLVMPIPE ...)`, GL 4.3 core, ES 3.2), `peglgears`, and
`gpu/tools/drm-gbm-test.c` (GBM device and buffer, dmabuf export, EGLImage on
EGL's GBM platform, GL ES into it, readbacks: all 12 checks); under
tools/node-machine.js (the machine's kernel and rv in wasm, gpuserver, the stub
executor) the same test's plumbing, and `es2gears_wayland` presenting GPU
resources by dmabuf (slot format 2, two buffers in turn, 373 presents in 40 s,
no readback).

## Stage C: Vulkan (venus) — not now

Measured: venus over vtest runs in the guest (vulkaninfo above). The host
side is the problem: venus' memory is host-visible and coherent, mapped
into the guest (blobs), and its command stream is Vulkan; WebGPU has no
mapped memory, no SPIR-V (WGSL only), no geometry/tessellation, a different
binding and barrier model, and nothing exists that translates Vulkan to
WebGPU (gpu-host's and our research agree; sources). Venus' ring and blobs
would fit this ring's shape (the venus command ring is itself a blob in
shared memory the executor could read in place); the translation is the
cost: months, not weeks.

## Effort (agent-days, estimates)

| item | who | days |
| --- | --- | --- |
| gpuserver: vtest server side, backing files, ring, completion, caps, `desktop` integration | gpu-guest | 3–5 |
| vrend to wasm on WebGL2, the executor, caps, transfers (es2gears/glxgears) | gpu-host | 10–20 |
| stage A working end to end in the browser (glxgears in a Wayland window) | both | +2–4 |
| wlserver: `zwp_linux_dmabuf_v1` + export table + slot format 2 | gpu-guest (with wlserver's owner) | 2–3 |
| rv-virtgpu (DRM render node emulated in rv over vtest) | gpu-guest | 8–15 |
| stage B zero-copy composition in the page (slot format 2, `gpu_serial`) | gpu-host | 3–6 |
| real programs (Qt Quick, GTK4, Chromium GPU process) to usable | both | weeks; unknown |
| venus on WebGPU | — | months; not planned |

## Sources

- Mesa vtest winsys: `src/gallium/winsys/virgl/vtest/virgl_vtest_socket.c`,
  `virgl_vtest_winsys.c` (mesa-25.0.7), selected by `GALLIUM_DRIVER=virpipe`
  in `src/gallium/auxiliary/target-helpers/sw_helper.h`.
- Mesa venus vtest backend: `src/virtio/vulkan/vn_renderer_vtest.c`; WSI
  assumptions: `src/virtio/vulkan/vn_wsi.c`; Wayland WSI's dmabuf/shm
  choice: `src/vulkan/wsi/wsi_common_wayland.c` (`registry_handle_global`).
- virglrenderer `vtest/vtest_protocol.h`, `vtest/vtest_renderer.c` (main).
- Mesa docs: venus (vtest: `virgl_test_server --venus`, `VN_DEBUG=vtest`;
  host requirements) https://docs.mesa3d.org/drivers/venus.html ;
  d3d12 (GL 3.3 on D3D12) https://docs.mesa3d.org/drivers/d3d12.html .
- WSLg: d3d12 gallium driver over Microsoft's libd3d12/libdxcore and the
  dxgkrnl kernel driver (`/dev/dxg`), i.e. a kernel driver plus a Windows
  host: https://devblogs.microsoft.com/directx/directx-heart-linux/ ,
  https://lpc.events/event/9/contributions/610/attachments/700/1295/XDC_-_WSL_Graphics_Architecture.pdf
- gfxstream kumquat (virtio-gpu over a Unix socket, guest Mesa built with
  `-Dvirtgpu_kumquat=true`, host needs EGL+GLES+Vulkan):
  https://github.com/magma-gpu/rutabaga_gfx ,
  https://source.android.com/docs/devices/cuttlefish/gpu ,
  https://github.com/google/gfxstream/pull/168
- virglrenderer on GLES hosts, guest version follows host:
  https://static.linaro.org/connect/lvc21f/presentations/LVC21F-218.pdf ,
  https://www.collabora.com/news-and-blog/blog/2025/01/15/the-state-of-gfx-virtualization-using-virglrenderer/ ;
  vrend's per-feature GLES minimums: `src/vrend/vrend_renderer.c` (`FEAT()` table);
  UTM guests on ANGLE/Metal (ES 3.0) report GL 2.1:
  https://github.com/utmapp/UTM/issues/7691
- Prior art in a browser: wasm-vm's virgl shader translator in wasm and a
  replaying decoder, not live: https://github.com/BLamy/wasm-vm/pull/403 ,
  https://github.com/BLamy/wasm-vm/pull/411 . No Vulkan-on-WebGPU layer found
  (closest an RFC without code: https://github.com/Tencent/ncnn/issues/5974).
  ANGLE's WebGPU backend exists in source, caps are placeholders:
  https://chromium.googlesource.com/angle/angle/+/refs/heads/main/src/libANGLE/renderer/wgpu/
