# A real GPU for the machine, on the host's WebGPU

Branch `webgpu`, 2026-10-10. Decision, evidence, the gaps, the spikes, and a
work breakdown for several agents. Spike code and raw results are in
`gpu/webgpu/spikes/`. Marks: **[measured]** = run here, on this Mac or in
the build VM; **[source]** = read in code, specs or docs (links at the end);
**[inferred]** = my reasoning, not checked.

## The decision in one paragraph

Put the GPU driver **in the page, not in the guest**. Write one Vulkan
driver on WebGPU, called **wgvk** here. It is a Mesa Vulkan driver built on
Mesa's common Vulkan runtime and compiler (NIR), and it emits WGSL. It is
compiled to wasm and calls the browser's WebGPU through Emscripten's
`webgpu.h` (emdawnwebgpu). Two front ends feed it, both in the same page
Worker, and both reach the page over the transport we already have (vtest
socket → gpuserver → ring in shared memory):

- **GL / GLES apps.** They keep stock Debian Mesa's **virgl** driver in the
  guest, the thin encoder that already runs today. In the page, a new virgl
  decoder ("vfront") turns virgl's Gallium-level commands into Gallium calls
  on Mesa's **zink**, and zink runs on wgvk. This replaces vrend-on-WebGL2.
- **Vulkan apps.** They use stock Mesa **venus** in the guest. In the page,
  virglrenderer's venus decoder (**vkr**) runs on wgvk.

The guest's Mesa stays the stock Debian package. Every expensive step runs
in the page at close to native speed:

- shader translation,
- GL state to Vulkan,
- Vulkan to WebGPU,
- emulation of missing stages (geometry, tessellation, transform feedback)
  in compute, using Mesa's libpoly.

That puts GL 4.3 within reach (Blender 4.3 needs it), along with Vulkan 1.1+
and, through them, Chromium, Qt and GTK on the GPU.

## The picture

```
 guest (riscv64 under rv; stock Debian trixie Mesa 25.0.7)
   GL/GLES program --> Mesa GL --> virgl (Gallium -> virgl stream, TGSI text)  ─┐  vtest over
   Vulkan program  --> venus (Vulkan -> venus stream in a shared ring/blobs)    ─┤  $XDG_RUNTIME_DIR/rvgpu-0
                                                                                │
 gpuserver (JS process in the machine; exists, gpu-guest's)  <──────────────────┘
   vtest server side; resource backings and venus blobs = memfds in shared linear memory;
   ring v2 (+ new ops for venus) ; completion words ; export table (dmabuf -> window slot)
                         │  ring + doorbell in the machine's SharedArrayBuffer
 page Worker "rvgpu" (one wasm module, one GPUDevice)
   vfront: virgl decoder -> Gallium (pipe_context) of zink (Mesa, page-side build)
   vkr:    venus decoder (virglrenderer src/venus)
              │ Vulkan
   wgvk: Mesa Vulkan driver on WebGPU
     objects/memory/descriptors -> GPUBuffer/GPUTexture/bind groups; cmd buffers -> encoders
     shaders: SPIR-V -> spirv_to_nir -> WebGPU legalisation passes -> NIR -> WGSL
     GS / tessellation / transform feedback: libpoly (compute) ; points/lines/fans etc.: lowering
              │ webgpu.h (emdawnwebgpu)        [native test bed: the same code on Dawn native in the VM]
   browser WebGPU (Chrome: Dawn -> Metal on this Mac)
              │ frames: GPUTexture -> OffscreenCanvas -> ImageBitmap
 display.js (WebGL2 compositor, main thread; exists)
```

## Why this layering (the evidence)

1. **The guest CPU is the scarce resource, so the guest should only encode.**
   The workload: the same GL ES 3.0 program, 512×512, with uniform changes
   and a texture bind per draw. It ran under native rv (interpreter) against
   the same host, in two configurations: virgl, and zink on venus
   ([measured], spike S3, `spikes/results/s3-drawbench.out`):

   | guest stack | 100 draws/frame | 1000 draws/frame |
   | --- | --- | --- |
   | virgl (stock) | 4.4 ms CPU/frame (44 µs/draw) | 52 ms CPU/frame (52 µs/draw) |
   | zink + venus (stock) | 28.5 ms CPU/frame (285 µs/draw) | 101 ms CPU/frame on 2 threads (101 µs/draw) |

   A linear fit says zink+venus costs about 1.5× virgl's guest CPU per draw
   (81 vs 53 µs), plus about 20 ms of guest CPU per frame of fixed overhead
   (interpreter time; the JIT divides both).

   One caveat: venus waits for the host by spinning (`vn_relax`), so part of
   that fixed cost may be spin-waiting rather than work [inferred, not
   profiled]. On this machine a spinning guest thread still burns an
   emulated CPU.

   So GL should cross the boundary as virgl, and zink should run on the page
   side.

2. **Shader translation in the page is cheap. In the guest it would not be.**
   Mesa's `spirv_to_nir` plus a driver-style NIR optimisation loop ran over
   all 177 SPIR-V modules that Blender 4.3 produced through zink. Built with
   Emscripten it took 1.85 ms per module in wasm, against 1.2 ms per module
   natively (1.55×). All 177 succeeded ([measured], spike S4). The same
   compiler work inside the guest runs at rv's speed. The only figure we have
   for that is guest Node.js running a JavaScript benchmark 8 to 47 times
   slower than on the Mac ([measured] earlier in this project). For compiled
   C under rv's JIT the factor is unmeasured, but it is surely well above
   wasm's 1.55× [inferred].

   Blender alone compiles hundreds of such modules. This rules out the
   layerings that translate in the guest (B and C below).

3. **zink's needs on a WebGPU-shaped device are known.** A Vulkan layer
   (`spikes/wgmask`) made lavapipe advertise only what a WebGPU-backed
   driver could offer. zink, Blender 4.3.2 and Debian's Mesa 25.0.7 then ran
   on it ([measured], spike S1; details below). What we learned:
   - The feature set wgvk must offer, natively or emulated, for GL 4.3 is
     exactly listed.
   - Blender 4.3's UI, Workbench and EEVEE render correctly on a profile
     with no tessellation, no transform feedback, no fp64/int64 and one
     viewport. The conditions are `MESA_GL_VERSION_OVERRIDE=4.3`, geometry
     shaders present (they would be emulated), and at least 24 storage
     buffers per stage.

4. **SPIR-V straight into Tint does not work for Vulkan-style SPIR-V.**
   Tint's SPIR-V reader accepted none of the 177 zink modules ([measured],
   spike S2). The failures are arrays of buffer bindings, dynamically
   indexed UBO/SSBO arrays, push-constant arrays, `DrawParameters`, demote,
   and SPIR-V 1.6. So wgvk needs its own WebGPU legalisation at the NIR
   level, followed by a NIR → WGSL emitter (KosmicKrisp's NIR → MSL is the
   template). Tint stays only as an offline validator.

5. **The browser's WebGPU is fast enough to sit under a driver** ([measured]
   on this Mac in Chrome for Testing 154 through the test browser, spike S6,
   `spikes/results/adapter-probe-m4max.json`):

   | Measurement | Result |
   | --- | --- |
   | JS-side `setBindGroup` + `draw` | 0.054 µs per pair to encode |
   | 20 000 draws | encoded, submitted and finished in 10 ms |
   | `writeBuffer` | 2.7 GB/s |
   | submit → `onSubmittedWorkDone` | 0.07 ms p50 |
   | copy + `mapAsync` readback, 4 KB | 0.2 ms p50 |
   | copy + `mapAsync` readback, 8 MB | 0.42 ms p50 |

   **Pipeline creation is the expensive call:** 17 ms p50 for a small
   compute pipeline. Pipeline caching and asynchronous creation are
   therefore first-class work.

6. **Nothing reusable exists for the hard part** ([source], prior-art
   search). There is no mature Vulkan-on-WebGPU.
   - `VkWebGPU-ICD` (Rust, wgpu + naga) is triangle-level and has no
     geometry shaders, tessellation or transform feedback.
   - ANGLE's WebGPU backend reaches roughly ES 2.0/3.0 with no compute (one
     wasm build exists, `gles-on-webgpu`).
   - Mesa, by contrast, has every piece except the WebGPU backend:
     - the Vulkan runtime;
     - NIR and `spirv_to_nir`;
     - zink;
     - libpoly (`src/poly`), which emulates geometry shaders, tessellation
       and transform feedback in compute, and which the Asahi, Honeykrisp
       and KosmicKrisp drivers use;
     - KosmicKrisp (Vulkan on Metal), the closest existing driver to wgvk.
       WebGPU was designed largely after Metal.

## The layerings compared

| | A: venus → page Vulkan-on-WebGPU (+ zink in the guest for GL) | B: new guest Gallium driver → WebGPU stream | C: guest Vulkan ICD on WebGPU (+ zink in the guest) | D: Dawn wire (guest client, page server) | **Chosen: A for Vulkan + virgl → page zink for GL** |
| --- | --- | --- | --- | --- | --- |
| Guest Mesa | stock | **custom build for riscv64** | **custom build for riscv64** | Dawn client for riscv64, plus B or C on top for GL/Vulkan | stock |
| Where shaders are translated | page (wasm) | guest (rv) | guest (rv) | depends on what sits on top | page (wasm) |
| Guest CPU per GL draw | zink + venus: 1.5× virgl + fixed per-frame cost [measured] | Gallium driver: about zink's state work, plus WebGPU encoding [inferred] | zink + driver, the worst [inferred] | — | virgl, the cheapest [measured] |
| Vulkan for apps | yes | no | yes | no (WebGPU only) | yes (venus) |
| Transport | venus (exists in Mesa; async, client-side ids, ring and blobs in shared memory) | new | new, or Dawn wire | Dawn wire (designed for exactly this; client-side ids, async) | vtest + ring (exists) for both |
| Page-side work | vkr + wgvk | a thin WebGPU replayer | a thin replayer | Dawn wire server on emdawnwebgpu (no precedent) | vfront + zink + vkr + wgvk |
| Engineering | wgvk (large) + vkr port | Gallium driver (large) + replayer | wgvk (large) in the guest + replayer | small for WebGPU apps, but it is not a GL/Vulkan solution | wgvk (large) + vfront (medium) + vkr port |
| Verdict | the Vulkan half is kept; GL through guest zink stays as a fallback | rejected: guest CPU, guest Mesa build, no Vulkan | rejected: guest CPU twice over (zink + driver), shader compiles at rv speed | later, only if native WebGPU apps in the guest matter | **chosen** |

Also considered and rejected:

- **Keep vrend, but on GLES 3.1+ in the page** (ANGLE-on-WebGPU in wasm).
  ANGLE's WebGPU backend has no compute shaders, SSBOs, images or transform
  feedback, so it tops out below what vrend needs for GL 3.2+.
- **Patch the apps to talk WebGPU.** That only covers the patched apps, and
  the apps here are stock Debian builds.
- **A Gallium driver directly on WebGPU in the page**, with no zink and no
  Vulkan layer. It would duplicate zink (about 60 000 lines) and give no
  Vulkan.

The zink → wgvk pair costs one extra layer for GL. It runs at wasm speed,
and later it can hand NIR to wgvk directly instead of SPIR-V (an internal
shortcut, since both sit in the same module).

## What WebGPU lacks, and what wgvk does about it

Two sources feed this table:

- the real adapter on this Mac ([measured] S6): Chrome for Testing 154,
  `apple / metal-3`, subgroups of 32;
- the WebGPU/WGSL specs, gpuweb issues and Chrome release notes ([source]).

"zink needs" comes from spike S1 (which masks decided the GL version) and
Mesa's `docs/drivers/zink.rst`. "Blender needs" comes from Blender 4.3.2's
`gl_backend.cc`, plus S1.

Read the "wgvk" column as follows:

- **native**: WebGPU has it.
- **lower**: a NIR pass, or a draw-time fix-up in the driver.
- **emulate**: extra GPU work (compute passes or copies).
- **fake**: advertise it, implement it partly or not at all, and log.

| Need (who) | WebGPU here | wgvk | Priority |
| --- | --- | --- | --- |
| Geometry shaders (GL 3.2 core; **Blender's GL backend uses them unconditionally**; Blender's Vulkan backend requires them) | none (gpuweb #1239, closed) | emulate: libpoly runs VS and GS as compute and writes an index buffer and an indirect draw | M2 |
| Tessellation (GL 4.0; zink drops to 3.3 without it [measured]) | none (#445) | emulate: libpoly (TCS as compute, the tessellator in compute, TES as a vertex shader) | M3; Blender does not use it, so until then `MESA_GL_VERSION_OVERRIDE=4.3` |
| Transform feedback (GL 3.0 core; **zink drops to GL 2.1 / ES 2.0 without `VK_EXT_transform_feedback`** [measured]) | none | emulate: libpoly's passthrough GS writes the XFB buffers; fake (advertise, no output) until then | the extension is needed at M1 (fake); real at M3 |
| Draw parameters, `gl_BaseVertex`/`BaseInstance`/`DrawID` (zink 4.1 → 4.2+ [measured]) | no built-ins | lower: values passed through immediates per draw; indirect draws get a compute pre-pass | M1 |
| Vertex attribute divisor (GL 3.3; zink 3.2 without it [measured]) | step mode vertex or instance only | lower: vertex pulling (`buffer_view`) for divisors > 1 | M1 |
| Storage images without a format qualifier (GL 4.2; zink 4.1 without [measured]) | the WGSL texel format is mandatory | lower: shader variant keyed by the bound view's format | M1 |
| Extended storage image formats (zink 4.1 without [measured]) | tier1 + tier2 present [measured] | native for tier formats; others packed as r32uint with conversion in the shader | M1 |
| Uniform buffers per stage: zink needs ≥ 15 for GL 4.3 [measured] | 12 [measured] | lower: the default-uniform block goes to immediates or a driver UBO; extra UBOs packed into one buffer binding | M1 |
| Storage buffers per stage: **Blender needs 12, and Mesa reserves half for atomic counters, so the driver must offer 24** [measured] | **10** [measured] | lower: "buffer heap". SSBOs are suballocated from a few large GPUBuffers and passed as (heap, offset, size); shaders index through `buffer_view` with explicit bounds checks; read-only and writable heaps are kept apart because of usage scopes | M1 (Blender blocker) |
| Viewports: 16 for GL 4.1 (zink 4.0 with 1 [measured]) | 1 | fake 16, honour viewport 0; real per-viewport draw replay later | M1 fake |
| Stores and atomics in vertex shaders (Blender: vertex SSBOs; zink reports 0 vertex SSBOs without them) | read-only storage only, in the vertex stage | read-only use is native; a vertex shader that writes runs as compute (libpoly's software VS path) | M1 read-only, M2 writes |
| Bind groups: Vulkan sets, push constants, driver bindings | 4 groups; immediates 64 B [measured] | zink compact mode (≤ 4 sets); push constants ≤ 64 B go to immediates, more to a UBO ring; driver bindings appended to an app group | M0 |
| Combined image samplers, sampler arrays indexed dynamically | separate in WGSL; no binding arrays (flag only) | lower: split; turn the array into N bindings plus a switch | M1 |
| Texel buffers (GL 3.1 core) | none | lower: storage buffer plus format decode in the shader | M1 |
| fp64 (GL 4.0 core), int64 | none in WGSL | lower: Mesa's soft-fp64 and `nir_lower_int64` | M2 (Blender uses neither [source]) |
| Image atomics (Blender's EEVEE: `imageAtomic*` on r32ui/r32i, also in fragment shaders) | none (#4329) | emulate: r32 images that are used atomically live in a storage buffer and are copied to a texture for sampling | M2 (EEVEE blocker) |
| Logic ops (zink baseline; Blender uses XOR) | none | fake (warn) first; later a fragment-shader path through a copy | M3 |
| Polygon mode, wide lines, large points, stipple | none (1 px points and lines) | lower or emulate: lines and points expanded to quads (vertex pulling), line stipple in the fragment shader | M2 (Blender's overlays draw wide lines through geometry shaders, so these come with the GS work) |
| Provoking vertex "last" (GL default) | flat uses first | lower: `primitive_index` (present [measured]) to fetch the last vertex's value, or rotate indices | M1 |
| Primitive restart with any index; triangle fans and quads | fixed restart index; lists and strips only | lower: rewrite indices (compute or CPU) | M1 |
| Persistent, coherent host-visible memory (Vulkan core; venus's assumption) | only `mapAsync`; `writeBuffer` takes shared views | design: host-visible memory is the guest's shared pages. wgvk uploads what a command reads, when it reads it, and downloads what the GPU wrote before signalling a fence (below) | M0 |
| Synchronous readback and fences | asynchronous only; `mapSync` behind a worker flag | design: everything completes through words in shared memory (exists in ring v2) | M0 |
| Texture views with another format (GL 4.3 `ARB_texture_view`) | sRGB variants only | emulate: copy through a buffer, track coherence | M2 |
| 3-channel formats, D24 copies, depth sampling with filtering | none; D24 not copyable | lower: pad to RGBA; depth copies through a shader to r32float | M1 |
| Occlusion query counts, pipeline statistics, conditional rendering | occlusion only "non-zero"; the others none | native occlusion (counts in practice [inferred]); conditional rendering: compute zeroes the indirect arguments; statistics faked | M2 |
| Timestamps | `timestamp-query` at pass boundaries, 100 µs quantised | native at pass boundaries | M3 |
| Usage scopes (a buffer or texture cannot be writable storage and anything else in one render pass, or in one dispatch) | spec rule | design: track usage per pass; split render passes (load/store) or snapshot-copy on conflict; zink already ends passes for feedback loops | M1 |

Present on this adapter and used as they are [measured]:

- clip distances, dual-source blending, depth-clip-control;
- `texture-component-swizzle` (GL swizzle);
- float32 filterable and blendable, `rg11b10ufloat-renderable`;
- BC/ETC2/ASTC compression;
- texture-formats tier1 and tier2, `depth32float-stencil8`;
- shader-f16, subgroups (size 32);
- `primitive-index`, `indirect-first-instance`, timestamp-query;
- 1024 invocations and 32 KiB per workgroup;
- 16384² textures with 2048 layers;
- WGSL `buffer_view` and `immediate_address_space`.

Not present without flags: multi-draw-indirect (wgvk loops `drawIndirect`
when the CPU knows the count), `mapSync`, binding arrays.

## Spike results

All spike code is in `gpu/webgpu/spikes/`. The VM pieces (a Debian trixie
arm64 chroot with Blender 4.3.2 and Mesa 25.0.7, the same versions as the
guest) are built by `spikes/vm/trixie-chroot.sh`.

**S1: zink and Blender on a WebGPU-shaped Vulkan device**
(`spikes/wgmask`, `spikes/results/wgmask-p2-bisect.txt`).
- What `wgmask.c` is: a Vulkan layer that filters device extensions to an
  allow-list, masks core features from a list, caps the API version, and
  cuts limits to WebGPU's. It ran over lavapipe in the chroot.
- GL version zink gives, by profile:
  - full lavapipe: GL 4.6 / ES 3.2.
  - P0 (what WebGPU gives by bookkeeping alone): GL 2.1 / ES 2.0.
  - P1 (P0 + planned emulations + WebGPU limits): 3.0.
  - P1 with no limits: 4.1.
  - P2 = P1 + `shaderStorageImageWriteWithoutFormat`, with WebGPU limits
    except UBOs ≥ 15 and viewports ≥ 16: GL 4.3 / ES 3.2.
- What one item at a time does to P2:
  - Removing `VK_EXT_transform_feedback` gives GL 2.1.
  - `VK_EXT_vertex_attribute_divisor` gives 3.2.
  - Tessellation gives 3.3.
  - Draw parameters give 4.1.
  - Extended storage formats or write-without-format give 4.1.
  - `fragmentStoresAndAtomics` gives 4.1.
  - `robustBufferAccess` gives 4.2.
  - Dual-source blending gives 3.2.
  - Sample-rate shading and cube arrays give 3.3.
  - Independent blend gives 1.5.
  - Removing geometry shaders keeps desktop 4.3 but ES 3.1: Mesa does not
    check the GS stage for desktop GL 3.2.
  - Nothing else in P1's extension list changes the version.
- Blender 4.3.2 on P3 (no GS, tessellation, XFB, fp64 or int64; one
  viewport; override 4.3): it refuses ("Unsupported platform as it supports
  max 0 SSBO binding locations"), because zink exposes vertex SSBOs only
  with `vertexPipelineStoresAndAtomics`.
- Blender on P4 (P3 + geometry shaders + vertex stores + 24 SSBOs and 16
  UBOs per stage): the UI starts and draws (`results/ui-p4-small.png`).
  Workbench renders the same image as on full lavapipe. With 16 SSBOs per
  stage it rendered an empty frame; my reading is that Mesa then offers
  Blender only 8, the same number EEVEE's start-up warning printed at 16
  [inferred]. EEVEE Next renders the default cube, visually the same as on
  full lavapipe (`results/eevee-p4-small.png`).
- **Limitation:** lavapipe still executes everything. The layer only hides
  features, so "works on P4" means "zink and Blender ask for no more than
  P4". It does not mean "WebGPU can run it". The second half is wgvk's job,
  and the SPIR-V that zink produced under P4 is its test corpus.

**S2: zink's SPIR-V into Tint** (`spikes/vm/tint-all.sh`,
`results/tint-zink-spirv.txt`).
- Tint was built natively from Dawn main (c943ea32). 177 modules went in
  and 0 were accepted.
- Categories: arrays of buffer bindings (64), dynamic indexing of SSBO/UBO
  arrays (76), push-constant arrays and blend_src layout (15), demote (10),
  internal compiler errors (5), DrawParameters (4), GeometryStreams (2),
  PointCoord (1).
- **Consequence:** legalise at the NIR level in wgvk and emit WGSL from NIR.
  Tint's SPIR-V reader is built for SPIR-V that already obeys WebGPU's rules.

**S3: guest CPU per draw, virgl vs zink+venus** (`spikes/drawbench`,
`spikes/vm/s3-cpu.sh`). Numbers in "Why", item 1.
- Setup: Debian riscv64 Mesa under native rv (interpreter), with trixie's
  `virgl_test_server` 1.1.0 in the chroot as the host (llvmpipe for virgl,
  lavapipe for venus).
- Both stacks run: zink on venus reports
  `zink Vulkan 1.3(Virtio-GPU Venus (llvmpipe))`, GL 4.6 / ES 3.2, from the
  guest's stock packages. That also tests the Vulkan half's transport.
- Ubuntu's virglrenderer 1.0.0 could not export host-visible memory for venus
  (`virgl_renderer_resource_export_blob ... failed (-22)`). Trixie's 1.1.0
  could. Venus needs host-visible memory exportable as an fd that the guest
  maps; wgvk's host-visible memory is shared memory to begin with.

**S4: Mesa's compiler in wasm** (`spikes/mesa-wasm`,
`results/spirv2nir-wasm.txt`).
- Mesa 25.0.7's NIR, `spirv_to_nir` and util were built with Emscripten
  6.0.11 into a 4.5 MB wasm. All 177 Blender modules were translated and
  optimised, at 1.55× native time.
- Mesa's meson refuses an Emscripten build ("ld.wasm does not support
  shared libraries"). The spike replays the native build's
  `compile_commands.json` with emcc, plus a one-line `detect_os.h` patch.
  The real build (track T0) needs a proper solution: a meson patch, or a
  generated ninja file.

**S5: Dawn native in the VM.** This is the native test bed: wgvk on
`webgpu.h` over Dawn → Vulkan → lavapipe, with no browser. The result is in
`spikes/results/dawn-native.txt`. The Dawn build needed
`-DDAWN_SUPPORTS_CXX_MODULES=OFF`.

**S6: the real adapter** (`spikes/adapter/adapter-probe.js`,
`results/adapter-probe-m4max.json`). Its numbers are quoted in the table
above and in "Why", item 5. The run with experimental Blink features
(`mapSync`, multi-draw-indirect, resource tables) did not happen. Its first
attempt landed on a page that had no `navigator.gpu` (an insecure context,
probably because the dev server did not answer). The retry was withdrawn
from the busy browser queue. Without flags, those three entry points are
absent [measured].

## What runs when (target, not promise)

| Milestone | Guest sees | Apps |
| --- | --- | --- |
| M1: wgvk core + vfront, no poly | GL 3.3–4.1 (4.3 with override) + ES 3.1, through virgl. Vulkan 1.1 through venus (no GS/tess) | glmark2, es2gears, Qt Quick (GL), GTK4 ngl, Chromium (ANGLE on GL ES 3.x: GPU raster and WebGL2), vkcube, vkmark |
| M2: buffer heap, image atomics, poly GS + VS-as-compute, wide lines | GL 4.3 (override; GS real) | Blender 4.3 UI + Workbench + EEVEE; Qt RHI Vulkan; GTK4 Vulkan |
| M3: tessellation, XFB, logic ops, the rest | honest GL 4.3+ / ES 3.2, Vulkan 1.2-ish | Godot 4 (Vulkan), SDL3 GPU, most GL 3.3 games |

Notes:

- Debian does not build Chromium for riscv64 ([source]). Chromium here is
  this project's own build, which is the `chromium` agent's.
- ANGLE's Vulkan backend needs provoking vertex, transform feedback (or
  vertex stores) and 12 UBOs per stage for ES 3.0 ([source]). Chromium on
  GL ES via virgl is therefore the earlier path.

## How it stays fast

- **Nothing waits on a round trip.**
  - Ring records are fire-and-forget. Completion is a word in shared memory
    per context (exists: `ctx_done[]`), and venus timeline syncs become
    words too.
  - The guest blocks only where the API blocks (`glFinish`, `glReadPixels`,
    `vkWaitForFences`). The page answers those after `onSubmittedWorkDone`
    or `mapAsync` (0.07 / 0.2 ms [measured]).
- **Memory: the guest's pages are the truth for host-visible memory.**
  - wgvk exposes discrete-GPU-like memory types:
    - DEVICE_LOCAL: GPU-only GPUBuffers and textures.
    - HOST_VISIBLE | HOST_COHERENT | HOST_CACHED: the gpuserver memfd that
      the guest has mapped and the page reads in place.
  - When a command reads host-visible memory (a copy source, a bound vertex,
    index or uniform range), wgvk `writeBuffer`s that range from the shared
    view before it (2.7 GB/s [measured]).
  - When the GPU writes host-visible memory, wgvk copies the range to a
    MAP_READ buffer and writes it back into the shared pages before the
    fence word moves.
  - zink and most Vulkan apps stage through copies on discrete GPUs, so the
    bytes moved are the bytes the app asked to move. Nothing scans memory
    for changes.
- **Pipelines are the expensive object** (17 ms p50 [measured]).
  - wgvk keys WebGPU pipelines by everything WebGPU bakes in (shader
    variant, vertex layout, blend, depth, topology, formats) and creates
    them with `createRenderPipelineAsync` as soon as the key is known.
  - It keeps a persistent WGSL / pipeline-key cache in IndexedDB. Dawn
    keeps its own blob cache per origin.
  - The guest side already avoids recompiling: the guest has Mesa's shader
    cache in `~/.cache/mesa_shader_cache`.
- **One device, one Worker.**
  - All guest contexts share one GPUDevice in one page Worker. That gives
    no WebGL-style context limit and no cross-context copies.
  - The display gets frames as ImageBitmaps; the import costs 0.02–0.04 ms
    on the main thread ([measured] by gpu-host for the vrend Worker path,
    same mechanism). Moving display.js to WebGPU later would not remove that
    copy: devices cannot be shared across threads.
- **Per-draw cost in the page.**
  - WebGPU encoding is 0.05 µs per bind + draw [measured]. The cost that
    matters is wasm: vfront + zink + wgvk.
  - Native zink is a few µs per draw [inferred]. Target: under 10 µs per
    guest draw in the page. The guest's own cost (virgl, 44–52 µs per draw
    interpreted) stays the bound.

## Work breakdown: parallel tracks

Every track works against the native test bed first: wgvk as a Vulkan ICD
on Dawn native → Vulkan → lavapipe in the build VM. They go to the browser
only for integration. The shared corpus is `spikes/` (the wgmask P4
profile, Blender's SPIR-V dumps, reference renders from full lavapipe).
Sizes are agent-days. They are rough, and my estimates have run long before.

| Track | What | Interface it owns | Depends on | Size |
| --- | --- | --- | --- | --- |
| **T0 build and test bed** (1 agent, first) | Mesa main (pinned) built twice from one tree: natively, and with Emscripten (fix meson's wasm shared-library refusal or generate ninja); emdawnwebgpu link; the page Worker host (`rvgpu-worker.js`: loads the wasm, owns the GPUDevice, gets the machine's memory); Dawn native in the VM; the wgmask P4 profile as a CI oracle; scripts in `gpu/webgpu/build/` | `wgvk_create_instance(const WGPUDevice)`; `rvgpu_ring_attach(base, size, sab)`; build products `rvgpu.wasm` / `rvgpu.js` | — | 5–8 |
| **T1 wgvk objects** (1–2 agents) | physical device (features and limits = the P4 table, plus emulated ones as they land), memory types (above), buffers (incl. the buffer heap), images, views, samplers, formats table, descriptor sets → bind groups (cached by content), pipeline layouts (≤ 4 groups + driver slots), queries, fences/semaphores/timelines → completion words | Mesa `vk_*` runtime objects; `wgvk_bo` = {GPUBuffer, offset, size, shared-memory window or none}; a documented binding remap (set, binding) → (group, binding) | T0 | 15–20 |
| **T2 wgvk commands** (1–2 agents) | cmd buffers via Mesa's `vk_cmd_queue`, replayed into GPUCommandEncoder at submit; dynamic rendering → render passes with splitting on usage-scope conflicts; draws and indirect draws; the pipeline key, cache and async creation; vertex input incl. vertex pulling; copies, blits, clears and resolves (WebGPU copies or `vk_meta` shaders); dispatch; upload-on-use and download-on-fence for host-visible memory | `wgvk_pipeline_key`; the replay loop; the memory sync points | T1 (in parallel against stubs) | 15–20 |
| **T3 shaders** (2 agents) | (a) NIR → WGSL emitter (structured control flow, types, builtins, split texture/sampler, storage textures, atomics, subgroups, f16, immediates); (b) legalisation passes: binding remap, push constants, UBO layout, binding arrays → switch, write-without-format variants, texel buffers, draw params, divisor, provoking vertex, int64/fp64, points, buffer heap access, image atomics. Corpus: the 177 Blender modules + dEQP-VK shaders; the oracle is Tint's WGSL validator (native) and pass counts | `wgvk_compile(nir, key) → {wgsl, binding table, required variant keys}` | T0 | 20–30 |
| **T4 libpoly** (1 agent; after T3a basics) | GS, VS-as-compute, transform feedback, tessellation through `src/poly`; 64-bit global pointers → heap offsets; draw orchestration (compute passes + indirect draw), with the kernels built by `mesa_clc` on the native side | adds stages to `wgvk_compile`; a draw hook in T2 | T2, T3 | 10–15 |
| **T5 venus front** (1 agent) | vkr (virglrenderer `src/venus`) in wasm on wgvk; blob export = gpuserver memfds; gpuserver: `CONTEXT_INIT`, `GET_CAPSET` (venus), `RESOURCE_CREATE_BLOB` for host-visible memory, `SYNC_CREATE/WRITE/WAIT/RESET`, `SUBMIT_CMD2`, and their ring ops; guest: `mesa-vulkan-drivers` installed and an ICD file (**orchestrator**: Debian root) | PROTOCOL.md v3: new ring ops + sync words (written with gpu-guest's owner) | T0, T1 for real runs | 8–12 |
| **T6 virgl front, "vfront"** (1 agent) | virgl stream → zink's `pipe_context` (about 60 commands, nearly 1:1; vrend_decode.c is the reference), TGSI text → NIR (`tgsi_text_translate` + `tgsi_to_nir`), sub-contexts, transfers on the existing ring ops, caps v2 from zink's pipe caps (the inverse of virgl_screen.c); zink patched to take wgvk's `vkGetInstanceProcAddr` directly; vrend-on-WebGL2 stays as the fallback when WebGPU is missing | ring v2 unchanged; caps v2 | T0 (zink on lavapipe natively first, then on wgvk) | 8–12 |
| **T7 present** (1 agent, part-time) | GPU windows: venus WSI and virgl resources exported through dmabuf → export table → slot format 2 (exists); the Worker blits the GPUTexture into its OffscreenCanvas, then `transferToImageBitmap` to display.js (as gpu-exec.js's grab path); pacing and `shown` acks | the existing slot / export-table contract | T2 | 4–6 |
| **T8 apps and testing** (1–2 agents, from M1) | native: dEQP-VK subsets, piglit on zink+wgvk, Blender headless renders vs the reference images, vkcube; browser: the same through `with-browser.sh`; app bring-up: glmark2, Qt, GTK4, Chromium flags, Blender (override, then honest); performance counters (per-draw wasm µs, pipeline-cache hits, bytes uploaded) | test lists and pass counts in `gpu/webgpu/STATUS.md` | M1 onwards | ongoing |

**Critical path:** T0 → T1/T2 + T3 → M1 (vkcube natively, then es2gears
through vfront in the browser) → buffer heap + image atomics + T4 GS → M2
(Blender).

**Calendar guess with 6–8 agents:** M1 in about 2–3 weeks, M2 in about
5–6 weeks, M3 later. Total 110–150 agent-days.

**Rules for the tracks:**
- Page-side Mesa is ours: a pinned Mesa main plus our patches in
  `patches/mesa-page.diff`. The guest's Mesa is never rebuilt.
- One owner per interface. Changes go in the owner's doc
  (`gpu/webgpu/INTERFACES.md`, created by T0).

## Risks, in order

1. **Usage scopes and hazards** (WebGPU's per-pass rules vs Vulkan barriers
   and GL feedback loops). Splitting passes and snapshot copies cost time.
   Measure early on Blender's overlay and EEVEE passes.
2. **NIR → WGSL completeness.** WGSL's uniformity rules: barriers in
   non-uniform control flow cannot be switched off; derivatives can. There
   is no recursion. Formats are strict. The 177-module corpus is the
   progress meter.
3. **Pipeline creation stutter** (17 ms each [measured]). Blender has
   hundreds of shaders, so async creation plus persistent caches are needed
   from M1.
4. **Wasm CPU per draw** (vfront + zink + wgvk). If it exceeds about
   10 µs, the fix is to skip SPIR-V between zink and wgvk and pass NIR.
5. **libpoly's assumptions** (64-bit global memory, OpenCL kernels built
   with LLVM): heap lowering is needed. KosmicKrisp ships poly only for
   tessellation, not GS. Asahi and Honeykrisp use it for GS.
6. **The SSBO count** (24 needed vs 10). The buffer heap is a design
   commitment, not an option: Blender will not start without it.
7. **Memory traffic** for big host-visible uploads (textures streamed every
   frame). Bounded by app behaviour; counted by T8.
8. **Protocol versions.** The guest's venus (Mesa 25.0.7) against
   page-side vkr (newer): vkr negotiates. virgl caps v2 are stable.

## Sources

- Mesa: `docs/drivers/zink.rst` (per-version requirements);
  `src/gallium/drivers/zink/zink_screen.c` (`PIPE_SHADER_CAP_MAX_SHADER_BUFFERS`:
  0 for the vertex stages without `vertexPipelineStoresAndAtomics`);
  `src/mesa/main/version.c`; `src/poly/` (libpoly: GS, tessellation and XFB
  in compute; used by `src/asahi`, `src/kosmickrisp`);
  `src/kosmickrisp/compiler/nir_to_msl.c`; `docs/drivers/venus.rst`;
  `src/virtio/vulkan/vn_renderer_vtest.c`;
  `src/gallium/drivers/virgl/virgl_screen.c` (caps are passed through from
  the host). Read at mesa-25.0.7 (cloned in the VM) and main 2026-10-09.
- virglrenderer: `src/venus/vkr_device_memory.c` (host-visible memory must
  be exportable as dma-buf or opaque fd; flushes are not forwarded),
  `src/vrend/vrend_decode.c`, `vtest/vtest_protocol.h`.
- Alyssa Rosenzweig, "Conformant OpenGL 4.6 on the M1":
  https://alyssarosenzweig.ca/blog/conformant-gl46-on-the-m1.html ; FOSDEM
  2026 libpoly talk:
  https://fosdem.org/2026/schedule/event/KZUJ9X-geometry_shaders_in_panvk_with_libpoly/
- KosmicKrisp: https://www.lunarg.com/lunarg-at-xdc-2025-kosmickrisp-overview/
- WebGPU spec https://gpuweb.github.io/gpuweb/ ; WGSL
  https://gpuweb.github.io/gpuweb/wgsl/ ; gpuweb issues #1239 (GS), #445
  (tessellation), #4329 (texture atomics), #1546 (wide lines), #263 (point
  size), #7290 (persistent mapping), #2217 (sync readback); Chrome "What's
  New in WebGPU" (immediates in 150, buffer_view in 153, texture tiers
  in 142, component swizzle in 143): https://developer.chrome.com/blog/
- Dawn: `src/dawn/native/Limits.cpp`; Dawn wire (`src/dawn/wire`,
  client-side ids, `MemoryTransferService`); emdawnwebgpu.
- Blender 4.3.2: `source/blender/gpu/opengl/gl_backend.cc` (GL ≥ 4.3, ≥ 12
  SSBOs in every stage, `geometry_shader_support = true`),
  `source/blender/gpu/vulkan/vk_backend.cc`;
  `draw/engines/eevee_next/shaders/*` (image atomics, 32 KiB shared, 1024
  invocations).
- ANGLE `src/libANGLE/renderer/vulkan/vk_renderer.cpp`
  (`getMaxSupportedESVersion`), `src/libANGLE/renderer/wgpu/`.
- Prior art: github.com/Dankular/VkWebGPU-ICD; github.com/xarray/gles-on-webgpu;
  v86 discussion #1611.
- This project: `gpu/FEEDBACK.md`, `gpu/PROTOCOL.md` (gpu-host / gpu-guest:
  ring v2, vrend on WebGL2 = GL 2.1 / ES 3.0); `docs/research-speed-2026-10-09.md`.
