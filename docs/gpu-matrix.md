# GPU app matrix: apps x {webgpu, vrend}

The regression matrix for the default switch from the vrend executor (WebGL in the page) to the webgpu executor (vfront/zink/vkr on
wgvk, WebGPU in the page): plan risk R3 says a regression affects every desktop GL program, so no app that works on vrend may fail on
webgpu. VK-17 (Vulkan program, wave 4). The table is made by `tools/gpu-matrix.sh`; its cells are the verdicts of `tools/gpu-nm.sh`.

## How it was made
- **nm+dawn.** Every webgpu cell is the app run in the machine under node-machine with the REAL rvgpu executor (`rvgpu.wasm`, the page's
  `rvgpu-worker.js`) on Dawn's Node bindings (the Mac's Metal): `tools/gpu-nm.sh APP`. No browser. The rvgpu stamp is in the footer of
  the table; the executor refuses an unstamped or dirty build. The machine is the page's own kernel/runtime/initramfs and the `release`
  disk; this tree's window server, GPU server and `tools/x/desktop` are on top, and `/bin/rv` is this tree's build (the dev server's stage
  initramfs still has the Oct 11 `rv` without PRIME import across opens, which mpv and Chromium on the render node need: VK-14. `tools/stage.sh`
  from this branch gives the page the new one; `gpu-nm.sh --no-fresh-rv` keeps the stage's).
- **vrend cannot run under Node.** vrend is virglrenderer on WebGL2 inside the page (`tools/www/gpu-exec.js`); Node has no WebGL, and
  headless browsers are not used here. So the vrend column is `n/a` for every GL app, and those cells are the owner's: one browser run of
  the same apps on each executor with `gpu/tools/gpu-matrix-guest.sh` (`the manual test sheets`, section VK-17). The two Vulkan
  apps do have a vrend-side answer under Node, because vrend has no Vulkan: `gpu-nm.sh --stub` is an executor with no venus capset, which
  is what the GPU server tells a guest when vrend is the executor; vulkaninfo and vkcube run that way as well and fall to lavapipe.
- **Programs the disk lacks.** The `release` disk has Chromium, mpv, and the Qt Quick libraries, but no `qml` and no GTK 4. Both come from
  Debian trixie riscv64 (`gpu/tools/matrix-bundle.sh` in the build VM: `qml` 6.8.2, libgtk-4 4.18.6 with gtk4-widget-factory, glmark2 2023.01;
  every other library they need is on the disk), as `m.tgz`; `gpu-nm.sh` fetches it into `build/gpu-matrix-pkgs`.
- **What PASS means** (the report of `gpu/tools/gpu-nm-report.js`, which also prints a `MATRIX_RESULT` line for the table): the executor started
  and the ring moved (submits > 0), and then the app's own check: es2gears' gears in the window's pixels and frames > 0; glmark2 completes with a
  Score; Qt Quick and Chromium print frames (`N frames in S seconds = F FPS`) and their window is drawn, not a flat colour, and
  changes between PNGs; GTK 4's window is drawn; mpv uses VA-API, every frame the decoder sent is drawn, and the test card is in mpv's window the
  right way up; vkcube chooses the Venus device (a GPU type) and presents 300 frames that show the cube; vulkaninfo lists Venus first.
- **fps in this table are not GPU frame rates.** The guest is an emulated riscv64 under node-machine and every app that prints a rate runs at
  about 10 fps there, whatever draws it (es2gears 10.0, the Qt Quick scene 10.0, the Chromium page 9.8). A drop of the GPU's speed would not show.
  The table can show that an app works, not that it is fast; the speed comparison needs the page (owner run).

## The table (nm+dawn, `tools/gpu-matrix.sh`)

| app | what | webgpu (nm+dawn, the real executor) | vrend |
| --- | --- | --- | --- |
| es2gears | es2gears_wayland (GL ES 2, Mesa demo) | PASS: 51 frames, 10.012 fps, 976 submits | n/a: vrend is WebGL in the page, not runnable under Node; the owner's run (VK-17 sheet) |
| glmark2 | glmark2-es2-wayland --off-screen (all scenes) | PASS: Score 1726, 1711736 submits | n/a: vrend is WebGL in the page, not runnable under Node; the owner's run (VK-17 sheet) |
| qtquick | a Qt Quick scene (QML, 60 spinning rectangles + text) | PASS: 183 frames, 10 fps, 1514 submits | n/a: vrend is WebGL in the page, not runnable under Node; the owner's run (VK-17 sheet) |
| qtquick-vulkan | the same scene on Qt's RHI Vulkan backend (QSG_RHI_BACKEND=vulkan, venus) | PASS (VK-27, run alone): 499 frames, 9.9 fps, 502 presents, Virtio-GPU Venus, 2232 submits | n/a: owner (with vrend there is no venus: Qt takes lavapipe) |
| chromium | Chromium on GPU compositing (cr-drm: RVGPU_DRM=1, ANGLE on Mesa's EGL) | PASS: 915 frames, 9.8 fps, 949 presents, 1961 submits | n/a: vrend is WebGL in the page, not runnable under Node; the owner's run (VK-17 sheet) |
| gtk4 | a GTK 4 program | PASS: 813 submits | n/a: vrend is WebGL in the page, not runnable under Node; the owner's run (VK-17 sheet) |
| mpv | mpv --hwdec=vaapi (the page's video path) | PASS: 51 presents, 57 video frames drawn (4.8 ms), 447 submits | n/a: vrend is WebGL in the page, not runnable under Node; the owner's run (VK-17 sheet) |
| vkcube | vkcube --wsi wayland (Vulkan, venus) | PASS: 300 presents, Virtio-GPU Venus, 819 submits | stub executor, no Vulkan (as vrend answers): PASS: llvmpipe |
| vulkaninfo | vulkaninfo --summary | PASS: 3.9 s, Virtio-GPU Venus, 33 submits | stub executor, no Vulkan (as vrend answers): PASS: 5.5 s, llvmpipe |

webgpu column: 8 PASS, 0 FAIL, 0 n/a of 8 apps. rvgpu stamp: vk-09-wgvk-device-type 0f45ce6f2bdb mesa 35b085c4c0 dawn c943ea32b6; tree vk-17-gpu-app-matrix b7affc8; run 2026-10-13 12:01.

## Notes on the cells
1. **vrend column.** `n/a` everywhere except the two Vulkan apps (see "How it was made"); the owner's run fills it.
   The Vulkan cells say what vrend gives: no Vulkan device in the guest, so vulkaninfo lists llvmpipe only (5.5 s, lavapipe's own start) and
   vkcube chooses llvmpipe (type Cpu). On webgpu both get the Venus device (wgvk, `DiscreteGpu`, 3.9 s for vulkaninfo): these two are gains,
   not regressions.
2. **es2gears.** 51 frames in 5.1 s (10.0 fps, the guest's speed); the window pixels: red 18.5%, green 4.7%, blue 3.8%, background 71.6%
   (the gears, 4 of 4 PNGs). 976 ring submits, 946 JSPI suspensions (about 19 a frame).
3. **glmark2.** All scenes complete, Score 1726. (VK-06 saw 1121 and VK-25 831 on the same executor at other loads: the score moves
   with what else the Mac runs, so it is not a regression measure.) 1,711,736 ring submits and 573,119 JSPI suspensions in the run.
4. **Qt Quick.** `qml` 6.8.2 with `gpu/tools/matrix/spin.qml`: 60 rotating gradient rectangles and a text. The GL context is
   `virgl (zink Vulkan 1.1(wgvk (WebGPU: device) (MESA_LLVMPIPE))) 4.3 (Compatibility Profile) Mesa 25.0.7` (the `MESA_LLVMPIPE` is the
   driver id wgvk reports to zink, not a software run). The window is drawn and animated (4 different PNGs of 4), 183 frames counted by
   the scene graph in the run, 1514 submits.
5. **Chromium (cr-drm).** `RVGPU_DRM=1 desktop wayland chromium` on `gpu/tools/matrix/anim.html` (120 CSS-animated boxes and a 2D
   canvas); ANGLE on Mesa's EGL over the render node, `--in-process-gpu`. 949 presents of GPU buffers (stage B: the window is made of the executor's
   textures, not of wl_shm pixels), the page's own count 915 frames in 98 s (9.8 fps), 8 of 8 window PNGs drawn and all different, 1961 submits. Needs
   the new `rv` (PRIME import across opens): see the first bullet of "How it was made".
6. **GTK 4.** `gtk4-widget-factory` 4.18.6 with `GSK_RENDERER=ngl` (GL ES 3 through the executor): window 1161x799, 110 presents, 4 different
   pictures of 4, 813 submits. It has no frame counter, so there is no fps for this row.
7. **mpv.** `mpv --hwdec=vaapi` on the 640x360 H.264 test clip, with the stand-in WebCodecs decoder (VK-14): 60 chunks, 57 frames sent and
   57 drawn by the executor (4.8 ms a frame, the RGBA path because dawn.node takes no VideoFrame), 0 skipped, 0 errors, 24 of 24 window PNGs
   show the test card the right way up. A real VideoFrame (`importExternalTexture`) and Chrome's decoder are the owner's (VK-14 sheet).
8. **vkcube.** `Selected GPU 0: Virtio-GPU Venus (wgvk (WebGPU: device)), type: DiscreteGpu`, 300 presents, 8 of 8 presented PNGs show
   the cube (teal 6.0%, light 6.1%); 819 submits, one JSPI suspension (the executor's start).
9. **Executor start.** `ready after` was 3.7 to 4.0 s in all the runs of the table (rvgpu.wasm instantiate and WebGPU adapter and device on Dawn);
   with the lazy start (VK-15) that delay lands on the first GPU program of a session.

## Regressions against vrend
No app that is runnable under Node fails on webgpu: 8 of 8 cells PASS. That does not clear the switch by itself, because the vrend side
of every GL cell has not been run (it cannot be, here) and the speed of any cell has not been compared (the nm guest runs every app at about 10 fps).
What the run does show, as differences to keep in view when the owner's vrend column comes in:

- **The vrend column is open.** Until the owner's run (`the manual test sheets`, VK-17) the claim is "works on webgpu", not "as good as vrend". Gate: every
  app that works on vrend works on webgpu, and no app is below 0.8 times its vrend frame rate. Earlier numbers from the vrend executor, for orientation
  only (a different Mac load, other scenes and sizes, from `gpu/FEEDBACK.md` and `the manual test sheets`): es2gears 55-60 frames/s, a 200-rectangle Qt Quick scene
  41-60 fps, a six-scene glmark2 score of 186, Chromium's local page 1.8-3.3 fps in stage A.
- **JSPI.** The webgpu executor suspends on `wgvk_wait` (VK-06 lists the eight places): about 19 times a frame in es2gears, 573,119 times in one glmark2 run.
  A browser without JSPI does not get webgpu (the VK-13 fallback chain keeps it on vrend), so it is not regressed, and the owner's frame rates show what the
  suspensions cost in Chrome.
- **First GPU program.** The executor needs 3.7-4.0 s to be ready (note 9); vrend has no such start. The owner sheet of VK-15 asks for the number in the page.
- **A new rv in the initramfs.** Chromium on the render node and mpv's EGL import need the `rv` with PRIME import across opens. The dev server's stage initramfs still
  has the old one (VK-14). `tools/stage.sh` from this branch has to run before the owner's run and before any release (VK-20).
- **GL version.** The guest's GL is now 4.3 compatibility / ES 3.x (vrend gave ES 3.0 / GL 2.1): programs that look at the version may take other code paths. The
  Qt Quick scene and GTK 4 (ngl) ran fine on it.

Tried, outside the matrix (the design's M2 list: "Qt RHI Vulkan; GTK4 Vulkan"), and failing, so they are follow-up cards, not part of the gate:
- **GTK 4 on the Vulkan renderer** (`GSK_RENDERER=vulkan`): GTK finds the Venus device and then aborts in `gdk_display_create_vulkan_device`: `assertion failed:
  (features & GDK_VULKAN_FEATURE_YCBCR)` (gdkvulkancontext.c:1596). The device does not offer sampler YCbCr conversion (Vulkan 1.1 `samplerYcbcrConversion`), which GTK 4.18 requires.
- **Qt Quick on Vulkan** (`QSG_RHI_BACKEND=vulkan`), fixed by VK-27, now the `qtquick-vulkan` row: (1) Qt 6.8's Wayland platform dlopens the unversioned loader
  name ("vulkan": libvulkan.so, libvulkan, vulkan.so, vulkan; `qemu-riscv64 -strace`), never libvulkan.so.1, and the disk has no libvulkan-dev link: `desktop`
  now exports `QT_VULKAN_LIB=libvulkan.so.1`. (2) The "segmentation fault in rv" was Qt's own: with no instance Qt calls `QVulkanInstance::isValid()` on NULL
  (libQt6Gui+0x48082c, `ld a5,0(a0)`, a0 = 0); rv only reports the guest's unhandled SIGSEGV, at the same pc under qemu-riscv64, the native rv interpreter and
  the wasm rv with and without RV_JIT. (3) Then the executor died: Qt's RHI draws through secondary command buffers, and wgvk's `vkCmdExecuteCommands`
  (Mesa's `vk_common_CmdExecuteCommands`) used `command_dispatch_table`, which was NULL (a `call_indirect` signature mismatch in the ring drain); wgvk now
  sets it to its recording table (t2 case `secondary`).

Follow-up cards suggested (also in the Board log):
1. **VK-17b: the owner's browser run of this matrix on both executors** (the sheet exists), then fill the vrend column here and decide the gate.
2. **wgvk: advertise `samplerYcbcrConversion`** (and `VK_KHR_sampler_ycbcr_conversion`) so GTK 4's Vulkan renderer starts on Venus.
3. ~~Qt RHI Vulkan on the machine~~: done in VK-27 (the `qtquick-vulkan` row).
4. **A speed signal that does not depend on the emulated guest:** the executor's time per frame or per submit (`rvGpu.stats.exec`) in the table, so a slower wgvk shows under Node.

## Running it again
- `tools/gpu-matrix.sh` (all apps; one node-machine at a time under `tools/nm-slot.sh`, so it waits for a slot), `tools/gpu-matrix.sh vkcube mpv` for some, `--table-only` to rebuild the table from `build/gpu-matrix`,
  `--out FILE` to write it. Needs the build VM for the riscv64 programs the first time (`gpu-nm.sh` header) and a dev server on :8000 for
  kernel, runtime, initramfs and disk. Raw reports of this run: `docs/gpu-matrix-runs/`.
- In the page (owner): `the manual test sheets`, section VK-17: `bash /run/rvgpu/matrix/gpu-matrix-guest.sh all` with `?exec=webgpu`
  and again with `?exec=vrend`. Fill the vrend column from that run, and the fps comparison (the gate: no app below 0.8 times its vrend rate).
