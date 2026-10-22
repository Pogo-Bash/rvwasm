# GPU on WebGPU: status

Pass counts per track. Numbers are measured natively in the build VM unless a
line says otherwise; each names the command that produced it (paths below
are relative to `gpu/webgpu/mesa/src/webgpu/compiler/`, run through
`gpu/webgpu/build/vm.sh`).

## VK-04: reproducible, stamped rvgpu.wasm (2026-10-12)

`gpu/webgpu/build/wasm.sh` builds `rvgpu.{js,wasm}` byte-reproducibly and stamps the wasm:

- **Stamp.** The last step appends the wasm custom section `rvgpu-stamp` (sorted-key JSON: `branch`, `commit` (full sha),
  `mesa` and `dawn` (the pins), `patch` (sha1 of `patches/mesa-page.diff`), `dirty`, `stamp: 1`); no time, no path.
  `gpu/webgpu/build/vm.sh` reads branch/commit/dirty on the Mac (the VM copy has no `.git`) and sends them as
  `RVGPU_BRANCH/COMMIT/DIRTY`. `dirty` is true when anything under `gpu/webgpu` or `patches` has uncommitted changes. A
  `wasm.sh` run without them (directly in the VM) stamps `commit: unknown, dirty: true`. Commit before building a build
  you mean to deploy.
- **Reader.** `node tools/rvgpu-stamp.js FILE.wasm` prints it (exit 1 when unstamped); `--json`; `--require-clean` exits 2
  unless the stamp is present, `dirty: false` and the commit is a full sha. `--write` is the (idempotent) writer.
- **Worker.** `tools/www/rvgpu-worker.js` compiles the wasm itself (`instantiateWasm`), logs `rvgpu: build <branch> <commit>
  [DIRTY] mesa .. dawn ..` (or `UNSTAMPED`) at start, sets `stats.build` (so `rvGpu.stats.exec.build` on the page) and sends
  `build` in the `ready` message.
- **Reproducibility.** The compilers get `-ffile-prefix-map` for `$HOME`, the shared dir, the worktree copy, the build dir and
  the Mesa tree (absolute and meson's relative `../../../trees/NAME` form), and `SOURCE_DATE_EPOCH` is fixed, so the bytes do
  not depend on the build name or worktree. Checked: two clean builds under different names (`vk-04a`, `vk-04b`) give
  identical `rvgpu.js` and `rvgpu.wasm` (sha256 in the log of the card).
- **Deploy rule.** Only a stamped, clean `rvgpu.wasm` goes to the site: `node tools/rvgpu-stamp.js --require-clean rvgpu.wasm`
  must exit 0. VK-05 makes `docker/release.sh` / `pack-www.sh` run it and refuse otherwise (and record the stamp in RELEASE.txt).

## VK-01: `webgpu` catch-up (2026-10-12)

What `webgpu` now holds that it did not at bf7cabc: `gpu-t2` a3e1c15 merged (indirect GS draws through libpoly's
setup kernel, `gl_ViewportIndex` / `gl_Layer` emulation, early depth tests, writeMask 0 on targets the fragment
shader does not write). One conflict, `vulkan/wgvk_private.h` (both fields kept: `mem_images` of T7, `gs_setup`
of T2). All numbers below are native in the build VM (`gpu/webgpu/build/vm.sh`, a wgvk-only build for check.sh,
a zink build for vfront/zink/wsi), before = webgpu bf7cabc, after = the merge.

| check | before | after |
| --- | --- | --- |
| `build/check.sh` (compiler, icd, legal, objects, ring, smoke-clear, t2, wsi; 150 ok lines) | 10 tests, 0 failed | 10 tests, 0 failed, same lines (only the wsi-x11 grey-sample count moves, 1828 to 1859) |
| `vfront.test`, `zink.test`, `wsi.test` (zink build) | not run | all ok; vfront-wgvk glm-small 468 of 468 images within tolerance, 456 bit for bit |
| `vkr/scripts/t2-venus.sh` (t2-test through venus vs lavapipe) | not run | 38 outputs compared, 0 differ |
| Blender 4.3.2 Workbench (wgvk-main vs lvp-main) | 22 of 2073600 px differ by more than 8 (max 19) | same |
| Blender EEVEE 64 samples | 2934 px (0.14%, max 54) | same, pixel for pixel equal to before (the default cube scene does not show the shadow) |
| EEVEE passes, Shadow | 82.16% of pixels differ (the GS draw was skipped) | **0.53%** (`SHADOW_POOL=64` or 16; see below) |
| EEVEE passes, Environment / Position | 18.61% / 15.52% | **0.00% / 0.57%** (early depth tests) |
| EEVEE passes, others | AO, Diffuse, Normal, Image within 0.6% | the same, within 0.6% |

The EEVEE shadow pool must be set for wgvk on lavapipe: Blender's default atlas is a 512 MB storage binding,
more than Dawn's 128 MB maximum (`Binding size (536870928) ... larger than the maximum storage buffer binding
size`; before, the shadow draw was skipped, so it never bound it). `SHADOW_POOL=64` and `16` render (EEVEE
64 samples: 74 s and 32 s, 64 indirect GS draws); `128` is killed by the VM's OOM killer (11.9 GB, about 9.7 GB
of shared memory). `tests/blender/blender-run.sh` takes `SHADOW_POOL=MB`; `chroot-wgvk.sh` takes the build from
`WGVK_B` (default the old gpu-t2 build). Not tried in a browser (the page's limits may differ).

Build note: `vm.sh` rsyncs with the Mac's modification times, so ninja skips a source whose mtime is older than
the object (a merge brings files older than an earlier build): after a merge or a checkout, `touch` the changed
sources in `~/wt/NAME` or the VM tests the old code. (First "after" runs here were stale for that reason.)

### `gpu-t5-int`: disposition of its 22 commits (`git log webgpu..gpu-t5-int`)

`gpu-t5-int` was a test integration of `gpu-t5`, `gpu-t5-server` and `gpu-t2`. 18 of the 22 are merges whose
second parents are all ancestors of `webgpu` (checked with `git merge-base --is-ancestor`): nothing to port.
The other 4 are single commits:

| commit | what | decision |
| --- | --- | --- |
| ccaf6b6 | wgvk_sync.c: a GUEST window's download straight from the mapped range | already on `webgpu` (arrived with `gpu-t5`: `retire()` has the same code and comment); nothing ported |
| 8fd8612 | `wgvk_wait` counts itself in `rvgpu_debug_words` ([2] waiting now, [4] waits) | **ported** by hand (b4e4fe6): weak symbols, no effect natively; the page's stuck report reads the words, so kept ("unsure, keep") |
| a33c003 | `wgvk_wait` writes what it waits for into `rvgpu_where` | **ported** with 8fd8612, same reason |
| 06f6300 | `rvgpu.wasm` with `--profiling-funcs` | dropped: grows the wasm (the tab is near V8's code-space limit) and is for profiling builds only; add the flag locally when profiling |

The `wgvk_wait` fix itself (wait only on the futures not yet completed) is on `webgpu` already.
Branch `gpu-t5-int` keeps its commits; nothing was deleted.

### Archive tags

`archive/<name>` tags (not branches; nothing deleted) on webgpu-e2e, gpu-t1, gpu-t3a, gpu-t3b, gpu-t4, gpu-t5,
gpu-t5-server, gpu-t6, gpu-t7 (all ancestors of `webgpu`) and gpufix, virtgpu (ancestors of `release`). Not tagged:
`gpu-t2` and `gpu-t5-int` (their tips are merge commits not in `webgpu`'s history, though their content is).

### Still open after VK-01

- image atomics: only r32 images used atomically (lowered to a storage buffer, T3b) are covered; EEVEE's shadow
  atlas goes this way and renders, other formats are untested;
- tessellation with a GS after it and indirect tessellated draws (T4, "Not yet" in its section); transform
  feedback with indirect GS draws is untested;
- EEVEE: the Shadow pass is within 0.53% but the 512 MB atlas of the default pool cannot be bound on Dawn's 128 MB
  limit (needs a split atlas or a larger maxStorageBufferBindingSize), and the OOM at 128 MB; other AOVs not
  looked at here; Blender's UI (Xvfb, workbench viewport) not run;
- the live site still runs an unstamped `rvgpu.wasm` from the VM tree `gpu-t5` (VK-04).

## T3a: NIR → WGSL (`src/webgpu/compiler`)

Measured 2026-10-10 on Mesa main 35b085c4 (the pin), Tint and Dawn c943ea32,
lavapipe of Ubuntu's Mesa 25.2.8 as the reference. `gpu/webgpu/tests/compiler.test`
runs the cs, draw, gfx and corpus checks for `build/check.sh`.

### Blender 4.3.2's 177 zink SPIR-V modules (spike S2's corpus, wgmask P4)

`tools/corpus.sh OUT [-remap] [-features]`: spirv_to_nir as a Vulkan driver
would, `wgsl_preprocess_nir`, `nir_to_wgsl`, then Tint's WGSL
reader/validator on every module that emitted. 70 compute, 53 vertex,
52 fragment, 2 geometry.

| | emit valid WGSL | blocked | emitted, invalid in Tint |
| --- | --- | --- | --- |
| before legalisation, no binding remap | **95/177** | 82 | 0 |
| with a stand-in for T1's binding remap (element i of a binding array → its own WebGPU binding) | **169/177** | 8 | 0 |

- The binding remap is not legalisation: it is INTERFACES 4.2's table, which
  `wgvk_compile` already reads. zink puts every UBO and SSBO in a binding
  array (`ubos[N]`, `ssbos[N]`), so without it every module with a second
  UBO or any SSBO is blocked.
- After NIR's optimisation every binding-array index in this corpus is a
  constant. Spike S2 counted 76 modules with dynamic indexing by SPIR-V
  capability; the indices fold away, so no module needs the
  switch/buffer-heap lowering for that.
- The corpus needs no optional WebGPU feature: the same 169 emit without
  f16, subgroups, clip distances, dual-source blending or primitive index.
- No emitted module goes over the M4 Max adapter's per-stage limits. The
  most any module uses: 8 storage buffers (limit 10), 5 uniform buffers (12),
  10 textures (48), 6 samplers (16), 7 storage textures (8).
- Cost natively (debugoptimized build, nir_validate on): front end 0.26 ms,
  `wgsl_preprocess_nir` 3.7 ms, `nir_to_wgsl` 0.21 ms per module.

**Blockers by construct: T3b's (and T4's) work queue.** "Alone" = the only
blocker of that many modules.

| construct (tag) | modules | alone | what removes it |
| --- | --- | --- | --- |
| `[binding-array]` element > 0 of a UBO/SSBO binding array | 75 | 74 | T1's binding remap (4.2), already consumed by `wgvk_compile` |
| `[int-texture-sample]` an integer texture sampled through a sampler | 3 | 3 | T3b: `textureLoad` at the nearest texel (wrap mode in the key) |
| `[stage]` + `[intrinsic]` geometry shaders (`emit_vertex`, `load_per_vertex_input`) | 2 | 0 | T4 (libpoly) |
| `[point-coord]` `gl_PointCoord` | 1 | 1 | T3b/T4: points as quads (`key.point_size`) |
| `[image-atomic]` image atomics (EEVEE) | 1 | 1 | T3b: r32 images used atomically live in a storage buffer (M2) |
| `[draw-params]` `gl_BaseInstance` | 1 | 1 | T3b: draw parameters through immediates (`key.draw_params`) |

### Second corpus: SPIRV-Cross's GLSL tests

`tools/xcorpus.sh OUT`: 169 of SPIRV-Cross's comp/frag/vert/vulkan/desktop-only/
flatten shaders that glslang compiles for Vulkan. **118/169 emit, all 118
valid in Tint.** Blockers (modules per tag): 64-bit types 11, non-32-bit
buffer access 12, unsupported intrinsics 11 (fragment interlock 4,
`load_input_vertex`/`load_barycentric_coord_pixel` 3+3, quad votes 2, ray
queries, view index), image atomics 7, `deref_cast` (buffer device address)
7, ALU ops on 8/16/64-bit packing and conversions 6, other texture ops 4 (sparse, fragment fetch), storage
cube/MS/buffer images 3, binding aliasing 3, 8-bit 3, rest one or two each.

### Runtime: our WGSL on Dawn native vs the SPIR-V on lavapipe

- `tools/cs-check.sh`: **9/9** compute tests (`tests/cs`: integer and float
  edge cases, control flow with phis and break/continue, shared memory with
  barriers and atomics including compare-exchange and barriers under
  conditions WGSL cannot prove uniform, UBO layouts, subgroups incl. ballot
  counts, push constants as immediates, f16 buffer loads, denormals) leave
  every storage-buffer word equal.
- `tools/draw-check.sh`: **3/3** vertex + fragment pairs (`tests/draw`:
  perspective / noperspective / flat / centroid interpolation with the
  interface linked from the fragment shader, every common sampling form in
  both stages, fragment loops) render the same 64x64 image, pixel for pixel.
  This is what found the clip-space y flip (see INTERFACES 6).
- `tools/blender-cs.sh`: Blender's buffer-only compute shaders (38 of the 69
  emitted; the rest use textures or images, which cs-run does not set up),
  buffers filled with a pattern, one workgroup, three reference runs to
  drop racy words. Of the 69 emitted compute modules, 31 use textures or
  images and are skipped. Of the 38 run: **32 leave every word equal**,
  4 ran longer than the time limit on pattern data (dump36, dump52, ui24,
  workbench04), 2 differ:
  - dump32, 1 word: an unsigned remainder by zero on pattern data (`32 >> n`
    with n ≥ 6). Vulkan leaves it undefined; WGSL defines `x % 0 = x`,
    lavapipe gave a value that sent the store out of bounds.
  - dump39, 68 words, NaN in Vulkan against 0 in ours: a `0 / v` where `v`
    sums products of storage-buffer words that are denormals as floats; the
    two compiles of lavapipe disagree on flushing them. dump51 differed the
    same way until uniform buffers were filled with normal floats only; it is
    equal now. Not proven for dump39 beyond reading the code path.
  (Assembled from two runs: the 60-second run gave 30 equal, plus dump37,
  whose Dawn readback had timed out in the harness, and dump51 after the
  pattern change, both rerun alone and equal.)

### Static checks of graphics shaders

`tools/gfx-check.sh`: `tests/gfx/` (every WGSL sampling form: implicit, bias,
level, gradient, offsets, arrays, cube, cube array, 3D, depth compare and
compare-level, gather with component and offset, gather-compare, texel fetch
of float/int/uint/multisampled, split texture + sampler, size/levels/samples
queries; derivatives; discard; frag depth; sample mask; vertex attributes of
three types; clip distances; storage images read/write/read-write; f16) all
emit WGSL that Tint accepts: **5/5**.

### Known gaps

- dEQP-VK is not built here; SPIRV-Cross's tests stand in for breadth.
- Rounding-mode conversions (`f2f16_rtz` …) are plain `f16()`; WGSL leaves
  the rounding to the implementation.
- WGSL lets an implementation assume there are no NaNs. `isnan` is a bit
  test and `fneu` is `!(a == b)`; other NaN-sensitive code is emitted as is.
- Graphics runtime coverage is three pipelines on one target format.

## T3b: WebGPU legalisation passes (`src/webgpu/compiler/wgvk_legalize.c`)

Measured 2026-10-10, same pins and reference as T3a. `wgvk_compile` runs
`wgvk_legalize_pre`, T3a's `wgsl_preprocess_nir`, `wgvk_legalize_post`, then
the emitter; `wgsl-corpus -layout -legalize` does the same for the corpora
(`-layout`: T1's binding remap built by INTERFACES 4.2's rules from the
shader's own declarations, `wgvk_layout_remap_from_nir`).
`gpu/webgpu/tests/legal.test` runs the checks below for `build/check.sh`.

### Progress meter: Blender's 177 modules, emit + Tint after the passes

| `tools/corpus.sh OUT ...` | emit valid WGSL | blocked | invalid in Tint |
| --- | --- | --- | --- |
| `-layout -legalize` | **177/177** (geometry shaders through libpoly, T4) | 0 | 0 |
| `-layout -legalize -features` | **177/177** | 0 | 0 |

T3a's queue, all cleared: integer textures sampled through a sampler (3,
textureLoad of the clamped texel), `gl_PointCoord` (1, a noperspective
varying the expanded point writes), image atomics (1, EEVEE's shadow atlas
as a storage buffer), `gl_BaseInstance` (1, immediates after the push
constants), binding arrays (74, the layout remap).

SPIRV-Cross's corpus (`tools/xcorpus.sh OUT -features -layout -legalize`):
**128/169** emit, all valid in Tint (T3a alone: 118). Left: intrinsics with no
WebGPU counterpart (fragment interlock, barycentrics, quad votes, ray
queries, view index), buffer device addresses (`deref_cast`), texture ops
(sparse, query LOD, weighted/box/block-match), cull distances, stencil export.

### Passes and their tests (`tools/legal-check.sh`, `tests/legal/`)

`legal-check.sh`: 19 shaders emit and pass Tint. The compute ones run on
lavapipe (SPIR-V, with the original descriptors: texel buffer views, R32
storage images, separate SSBOs) and on Dawn native (our WGSL, with what the
passes report: storage buffers, the image's header + texels, the heap arena
and table, driver UBO 900): **11 equal word for word** (bigpc, dynbuf,
dynpc, fatomic, heap, imgatomic, int64, mulhi, sfmul, small, texbuf).

| pass | what it does | test |
| --- | --- | --- |
| binding remap | (set, binding, element) → (group, binding) by T1's table; driver set 0xff → (driver_group, 900..999) | every test |
| push constants | ≤ immediates: as they are, dynamic offsets as selects; more: driver UBO 900 | bigpc, dynpc (run) |
| draw parameters | BaseVertex/BaseInstance/DrawIndex at align4(push constant bytes) in the immediates, else driver UBO 901 | drawparams (static) |
| dynamic binding-array index | textures, samplers, images, UBOs, SSBOs: binary tree of ifs over constant elements | dynidx (static), dynbuf (run) |
| integer texture through a sampler | textureLoad at the texel, clamped | inttex (static), 3 Blender modules |
| gl_PointCoord | from a varying (`info.point_coord_location`) | pointcoord (static), 1 Blender module |
| image atomics | r32 image → storage buffer {w, h, layers, 0; texels} | imgatomic (run), EEVEE dump62 |
| texel buffers | storage buffer + format decode/encode in the shader (`key.texel_buffer_format`) | texbuf (run) |
| formatless storage images | format from `key.storage_image_format[slot]` | formatless (static) |
| input attachments | textureLoad at the fragment's pixel | subpass (static) |
| 8/16-bit integers | arithmetic widened, buffer access as words (stores: atomicAnd/Or) | small (run) |
| int64 | `nir_lower_int64` | int64, mulhi (run) |
| fp64 | Mesa's soft-fp64 (float64.glsl → SPIR-V at build time) | fp64, fp64mul (static; see below) |
| float atomics | compare-exchange loops (buffers, shared) | fatomic (run) |
| UBO limit | UBOs past `caps.max_uniform_buffers` read as read-only storage | — |
| buffer heap | SSBOs past `caps.max_storage_buffers`: arena bindings 910+, table at 902 | heap (run: 12 SSBOs, 3 through the heap) |

### Graphics passes at run time (`tools/scene-check.sh`, `tests/scene/`)

`scene-run` draws a vertex + fragment pair and its draw (`tools/scene.h`: vertex
buffers, attributes, divisors, indexed or not, base vertex, first instance,
push constants, points) on lavapipe as SPIR-V with the real vertex input and
topology, and on Dawn native as our WGSL with what the passes ask for (pulled
buffers as storage, the index buffer of expanded points, draw parameters in
the immediates, points as 6 vertices each), and compares the 64x64 images.
**7/7 equal**:

| scene | what it checks | result |
| --- | --- | --- |
| divisor | per-instance divisors 2 and 0, R8G8B8_UNORM (pulled) next to a native attribute | 0 pixels differ |
| formats | R16G16B16_SNORM, R8G8B8A8_USCALED, A2B10G10R10_SNORM, R8G8B8_UINT, R16G16_SSCALED interleaved | 0 |
| drawparams | indexed draw, base vertex 5, first instance 2, 52 bytes of push constants + draw parameters in the immediates | 0 |
| points | 20 points of sizes 3..9, gl_PointCoord, pulled colour | 0 (74 within 1: gl_PointCoord interpolation) |
| pointsidx | the same through a 16-bit index buffer with a base vertex | 0 (51 within 1) |
| inttex | usampler2D / isampler2DArray through samplers: implicit LOD, textureLod, textureOffset | 0 |
| dynidx | dynamically indexed combined samplers, textures + sampler, UBOs, SSBOs | 0 |

`inttex` found that the integer-texture lowering read level 0 for implicit
LOD; it now takes Vulkan's level from the coordinate's derivatives (plus bias),
nearest mip level. The compute-side runtime list gained `formatless`
(formatless storage images with formats from the key: cs-run now runs storage
images), so **12 compute tests** match word for word.

### Emscripten build

`build/wasm.sh src/webgpu/compiler/libwgvk_compiler.a src/webgpu/compiler/wgsl-corpus.js
src/webgpu/rvgpu/rvgpu.js` builds, soft-fp64 step included (glslang runs
natively at build time). `wgsl-corpus.js` (the corpus driver as wasm under
node) on Blender's 177 modules with `-layout -legalize -features`: 175 emit,
**every WGSL file identical to the native build's**; front end 0.36 ms,
preprocess + passes 2.63 ms, emit 0.53 ms per module (wasm release vs native
debugoptimized with NIR validation: 0.26 / 4.41 / 0.23 ms). The fp64 and
int64 tests also give identical WGSL in wasm. wgvk does not link the
compiler yet (T1/T2), so rvgpu.wasm does not contain it.

### Known gaps

- fp64: soft-fp64's multiplies match lavapipe bit for bit; adds differ in the
  last bit in some cases (fp64mul: 43 of 1024 words), so the fp64 tests are
  static only. Blender uses no fp64.
- Provoking vertex "last" is meant to be T2's index rotation (a triangle's
  indices rotated keep its winding), so there is no shader pass.
- Expanded points: Vulkan discards a point whose centre is outside the clip
  volume, the quad is clipped instead (partly visible near the edges);
  gl_FrontFacing of an expanded point is not forced to true.
- Input attachments are checked by Tint only (scene-run has no render pass
  with subpasses). The integer-texture lowering assumes clamp-to-edge and a
  nearest mip mode, and ignores the sampler's min/max LOD.
- The heap's table and arenas are T1's to fill (INTERFACES 4.2 / 6).

## T4: geometry shaders through libpoly (`src/webgpu/poly`)

Measured 2026-10-10, branch `gpu-t4`. Mesa's libpoly (`src/poly`) is built
for wgvk from our own meson file: its OpenCL helpers through mesa_clc and
vtn_bindgen2 (from the integration build's native tools when the track build
has none), its NIR passes as a static library. `wgvk_poly.c` turns its
64-bit global addresses into segment:offset storage accesses (driver binding
940 for the poly buffer, 936 for the index buffer), its sysvals into driver
uniforms, its compute parts into 64-wide compute shaders with a grid bounds
check, and the vertex shader before a GS into compute (`key.vs_before_gs`).
INTERFACES.md 6.2 is the contract with T2.

- **Blender: 177/177.** The two geometry shaders (UI wide lines: lines to a
  4-vertex strip; EEVEE: triangles to a strip with `gl_ViewportIndex`, which
  is dropped: viewport 0 only) give a rast vertex shader, a main compute
  part (their topology is dynamic) and a pre part each; all 6 WGSL modules
  valid in Tint. `tools/corpus.sh`: 181 WGSL files, 0 invalid.
- **At run time** (`tools/scene-check.sh`, lavapipe running VS + GS + FS vs
  Dawn running VS-as-compute, the GS parts and the rast draw from libpoly's
  indirect draw command), all equal pixel for pixel:

| scene | GS | libpoly shape | parts run | result |
| --- | --- | --- | --- | --- |
| gslines | lines to quads (a 4-vertex strip in a loop), 6 lines | dynamic indexed | VS, count, pre, main, rast | 0 pixels differ |
| gstri | triangles to a strip with gl_ViewportIndex and gl_PrimitiveIDIn; indexed draw, base vertex 4, 3 instances from 2 | dynamic indexed | VS (reads the index buffer), pre, main, rast | 0 |
| gspoints | points to quads, straight-line code | static per primitive | VS, rast (drawIndirect) | 0 |

- The wasm build (`build/wasm.sh`) builds libpoly's generated C++ and the
  passes (the tools run natively at build time); `wgsl-corpus.js` under node
  gives the same 181 WGSL files as the native build, byte for byte.

Second step (same day):

| scene | what it checks | result |
| --- | --- | --- |
| vswrite | a vertex shader writing an SSBO (atomics, stores) run as compute + a passthrough vertex shader; indexed16, instanced, base vertex, first instance | 0 pixels differ (6 within 1) |
| blenderui | **Blender's own** polyline VS + GS + FS (UI wide lines) with its uniform layout (viewport size, line width) | 0 pixels differ, 623 drawn |
| blendereevee | **Blender's own** EEVEE shadow VS + GS + FS: no colour, atomicMin of depth into the shadow atlas (r32ui image used atomically: a storage image on Vulkan, the lowered buffer on WebGPU) | 0 atlas texels differ, 700 written |
| gslinesind | indirect draw through a GS (non-indexed): libpoly's setup on the GPU, every part dispatched indirectly | 0 pixels differ |
| gstriind | the same, indexed, 3 instances, first index 2 | 0 pixels differ |

- The indirect setup is our own OpenCL (`poly/wgvk_poly.cl`) on libpoly's
  `poly_gs_setup_indirect`, built by mesa_clc like libpoly; libpoly's
  printf/abort on heap overflow become nothing / a return.
- Blender corpus: 177/177, 183 WGSL files valid (the setup shader per GS too).
- scene files can take Blender's SPIR-V stages, buffer contents and push
  constant words; `wgsl-corpus -link -layout` builds one layout from all
  stages (each stage had its own before: a real pipeline has one).

Third step: **transform feedback** (GL 3.0 / ES 3.0 need it):

| scene | what it checks | result |
| --- | --- | --- |
| xfbvs | feedback from a vertex shader (no GS: libpoly's passthrough GS), two buffers, 3 instances | 0 words differ, 378 written; 0 pixels differ |
| gsxfb | feedback from a GS with a loop: counts not static, so count + prefix sum + pre + main + xfb parts | 0 words differ, 678 written; 0 pixels differ |
| xfbovf | xfbvs with buffer 0 too small: 7 of 12 primitives written | 0 words differ; query 7/12 on both |

- The feedback stores run in an **xfb part**: the rast part as compute, one
  invocation per output vertex of the rasterization draw (WebGPU vertex
  shaders cannot store); the rast part that draws has them stripped.
- Feedback buffers are segments 3+ (driver bindings 941+); the geometry
  params' `xfb_base*` pointers resolve to them by field.
- The prefix sum is our own CL, one invocation (libpoly's wants 32-wide
  subgroups); a parallel scan is later work.
- unorm8 attributes decode with a correctly rounded division; lavapipe's
  can be 1 ULP away (both within Vulkan's precision rules), so the feedback
  scenes use float attributes.

**Queries**: libpoly's pre part adds to `prims_generated_counter[stream]`
("needed") and `xfb_prims_generated_counter[stream]` ("written"); pointed at
words of the poly buffer, they equal lavapipe's
`VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT` results in every feedback scene
(12/12, 24/24, 7/12). T2 copies them into its query pool. Pipeline
statistics (GS invocations, clipper counts) go through the same pre part's
counters, not checked yet.

Fourth step: **tessellation** (libpoly's TCS/TES lowering and its D3D11
reference tessellator):

| scene | what it checks | result |
| --- | --- | --- |
| tessquad | quads, equal spacing, ccw; per-patch levels from gl_PrimitiveID, a patch output; 4 instances of 1 patch | 0 pixels differ, 2704 drawn |
| tesstri | triangles, fractional odd, cw; 3 patches per instance, 2 instances; a TCS reading another invocation's output after barrier(); a TES that reads back gl_Position | 0 pixels differ, 1057 drawn |
| tessiso | isolines (cubic Bezier per line), levels per patch | 0 pixels differ, 343 drawn |
| tesspts | point mode, fractional even | 0 pixels differ, 148 drawn |

- On Dawn: VS as compute, the TCS as compute (a workgroup of
  `tcs_vertices_out` invocations per patch), the count kernel, the prefix
  sum (1 invocation; allocates the index buffer from the heap and writes
  the draw), the tessellate kernel, then the TES as the vertex shader of a
  `drawIndexedIndirect` with the poly buffer as the index buffer.
- The tessellator is libpoly's CL imported at run time as an OpenCL library
  (vtn_bindgen2 hits its object limit on it). Its private memory lives in
  the poly buffer (80-400 bytes per invocation once dead variables are
  dropped before the layout; it was 19 KB with them), zeroed by the kernel
  first as WGSL zeroes function variables (the tessellator reads some
  before writing them); private reads that explicit IO folded into global
  loads of a private-tagged generic address go there too. The wasm build's
  kernels are byte-identical to the native ones.
  Pointers chosen by control flow (phis) keep their segment.
- Not yet: tessellation with a GS after it, indirect tessellated draws,
  transform feedback from a TES, and a parallel prefix sum.

**Multiview (T2's proposal)**: `gl_ViewIndex` reads driver uniform
`view_index`; scene multiview (view 2 of a three-layer target) equal.

Fifth step (T2's Blender bring-up, 2026-10-10), all equal to lavapipe:

| scene | what it checks | result |
| --- | --- | --- |
| blendereeveeind | **Blender's own** EEVEE shadow VS + GS + FS drawn indirectly (as EEVEE draws it), first vertex 3, first instance 1, the CPU's draw parameters poisoned | 0 atlas texels differ, 692 written |
| gstriind / gslinesind | indirect GS draws with base vertex / first vertex and first instance from the GPU command | 0 pixels differ |
| gsviewports | a GS routing triangles to 4 viewports (offsets, negative height, depth ranges incl. reversed, scissors) | 0 pixels differ |
| gslayers | a GS writing gl_Layer and gl_ViewportIndex, layer 1 of a 2-layer pass | 0 pixels differ |
| vsviewport | a vertex shader writing both per instance | 0 pixels differ |
| earlyd16 / earlyd32 | early fragment tests with image atomics, d16 LESS_OR_EQUAL / d32 GREATER | 0 texels differ (1216 / 1249 without the shader-side test) |

- Indirect GS draws: the setup kernel writes the draw parameters (base
  vertex, first instance, draw id) to the poly buffer, the compute vertex
  shader reads them there (EEVEE's resource offset is the first instance).
- `gl_ViewportIndex` / `gl_Layer`: position mapped from viewport i into the
  render area in the last vertex stage, a flat (viewport, layer) varying,
  the fragment shader discards outside viewport i / scissor i / the pass's
  layer. Limits: one index per primitive, depth clipped to 0..1.
- gl_PrimitiveID written by a GS (glxgears through zink, T7): dropped, or
  passed to the fragment shader as a flat varying when it reads it (scenes
  gsprimid, gsprimiddrop).
- Early fragment tests: the depth test in the fragment shader first
  (driver binding 938, read-only), as the format stores it; WGSL's discard
  stops the storage writes. Stencil tests are not emulated.

Not yet: GS instancing beyond what libpoly unrolls; draw parameters of
indirect draws from the GPU; primitive restart through a vertex shader run
as compute; multiple vertex streams at run time.


## T6: vfront, the virgl decoder (`src/webgpu/vfront`)

Measured 2026-10-10 in the build VM: vfront on Mesa main 35b085c4 (the pin) with
zink on lavapipe (the integration build's), guest side Debian trixie's Mesa
25.0.7 virgl driver (virpipe over vtest) in a chroot; reference
virglrenderer 1.1.0 (`virgl_test_server` on llvmpipe). Server:
`vfront/scripts/main-run.sh` (vfront-vtest outside the chroot, restarted if it
dies), tests `vfront/scripts/piglit-gl.sh`, comparison `vfront/tools/pgcmp.py`.

### piglit, GL 1.x-3.3 selection (quick profile, 8964 tests)

`spec@(glsl-1.(10..50)@execution|glsl-3.30|!opengl [123]|ext_transform_feedback|
arb_uniform_buffer_object|arb_texture_buffer_object|ext/arb_framebuffer_object|
arb_instanced_arrays|arb_draw_indirect|arb_shader_storage_buffer_object|
arb_compute_shader|arb_texture_multisample|arb_sample_shading|
arb_occlusion_query|arb_timer_query)`

| | pass | fail | crash | skip | timeout |
| --- | --- | --- | --- | --- | --- |
| vrend (virglrenderer on llvmpipe) | 8573 | 38 | 80 | 273 | 0 |
| **vfront** (zink on lavapipe) | **8556** | 57 | 157 | 193 | 1 |

- pass -> crash 47 and pass -> timeout 1: tests that were running when the
  server was OOM-killed once (the guest allocating past the VM's 12 GB;
  vrend's server was OOM-killed in its run too). All 48 pass when run alone,
  each against its own server (`vfront/scripts/main-each.sh`): 8604 pass
  with them (vrend 8573); 28 that pass on vrend fail on vfront (below).
- pass -> fail 28: arb_sample_shading with 2 samples (8; lavapipe has 1 and
  4, vfront rounds 2 up, which GL allows but these tests do not expect),
  3 SSBO (array-ssbo-binding, indirect), 64-bit float vertex attributes (4:
  no R64 vertex formats in Vulkan, not emulated), texture buffer formats (2),
  multisample positions (2), pbo (2), single others.
- skip -> pass 42, fail -> pass 19: vfront's caps (GL 4.3) expose more than
  vrend's on llvmpipe.

### Captures (tests/vfront.test)

`vfront/scripts/make-captures.sh`: glmark2-es2, 10 scenes at 160x120
(texture, shading, effect2d, pulsar, desktop, refract, conditionals, loop,
ideas, clear): 52 shaders, 1002 draws, 468 readbacks. Replayed into vfront:
468/468 readbacks equal the live run's; replayed into virglrenderer: 466/468
images equal (2 in `ideas`, coplanar shadows). Native replay costs about
9 us per command, 42 us per draw on this capture (most of it zink's first
pipeline compiles). Earlier, on Mesa 25.0.7: es2gears 5705/5713 frames equal
vrend's, glmark2-es2's 31 scenes 312/312 sampled frames equal, glsl-1.50
built-ins 1487/1487.

### On wgvk

Since T2 (ec7307a): the glmark2 capture replays fully on zink on wgvk on Dawn on
lavapipe, loader-free as in the page: all 468 readbacks within tolerance 8 of
the lavapipe run's images, 456 bit for bit (`tests/vfront.test`, vfront-wgvk).
In Chrome (Metal adapter) vfront inside rvgpu.wasm starts zink on wgvk
(`vfront/scripts/page-replay.sh`; before T2 it stopped at the first draw).

### Blender Workbench through vfront on wgvk (VK-12, 2026-10-12)

Blender 4.3.2 `-b -E BLENDER_WORKBENCH -f 1` (1920x1080) in the trixie chroot on the guest's Mesa 25.0.7 virgl driver
(`GALLIUM_DRIVER=virpipe`, vtest socket) into vfront-vtest on zink on wgvk on Dawn on lavapipe (build vk-03 = webgpu
88caffc's sources), against the lavapipe render (zink on the chroot's lavapipe): **22 of 2073600 px differ by more
than 8/255 (0.001%, max 19)**, the same 22 px as direct zink on wgvk (VK-01), five runs identical. Blender is a vtest
context of the server (`blender is context 1`) and a capture of the run is 1 MB. 1.4-1.8 s per render, server peak
RSS 326-413 MB (no leak to speak of in a single render; the wgvk server's leak shows over thousands of piglit tests).
vfront on zink on lavapipe is bit-exact with the direct lavapipe render (0 px). No wgvk gaps hit. EEVEE through vfront
is VK-22. `tests/blender/blender-vfront-check.sh` runs it (PASS/FAIL).

### GL 4.3 on wgvk, and piglit on wgvk (VK-03, 2026-10-12)

Measured in the build VM on branch `vk-03-vfront-gl43` (webgpu bf7cabc + vfront caps): vfront-vtest with zink on
wgvk on Dawn on lavapipe (`main-run.sh --vk wgvk`, `VFRONT_BUILD=vk-03`), guest side the trixie chroot's Mesa
25.0.7 virgl (virpipe over vtest).

**Version.** Before, the guest got GL 3.3 core: Mesa's `compute_version` wants, for 4.0, ARB_tessellation_shader and
(for 4.1) ARB_viewport_array, which wgvk's Vulkan does not offer. Now `eglinfo -p surfaceless` through vfront on wgvk
prints `OpenGL core profile version: 4.3 (Core Profile) Mesa 25.0.7`, GLSL 4.30 (compatibility profile 4.3 too), and
`glxgears -info` (GLX, Xvfb) `GL_VERSION = 4.3 (Compatibility Profile)`. `glxinfo` itself cannot be used: on virpipe it
dies with an X_GetImage BadMatch (an unmapped window), and virgl_test_server's own virpipe does the same; `scripts/gl-info.sh`
runs the two that work.

**Caps that cannot be honest** (`vf_caps.c` `gl43_fakes`, logged at start as `caps: faked for GL 4.3`;
`VFRONT_CAPS=honest` leaves them out and the guest is back at GL 3.3):

| cap | wgvk | what vfront does with it |
| --- | --- | --- |
| tessellation (`has_tessellation_shaders`, GL 4.0) | no `tessellationShader`: the compiler lowers TCS/TES (libpoly, T4) but the pipelines do not carry them | TCS/TES objects are accepted and dropped, draws of patches skipped (each logged once); limits at GL 4.0's minimums (32 TCS/TES outputs, 30 patch varyings). 27 piglit tests (`vertex-program-two-side`) use it and fail |
| 16 viewports (`max_viewports`, GL 4.1) | `maxViewports` 1 | viewport and scissor 0 reach the device, the rest are accepted and dropped |
| fp64 (`has_fp64`, GL 4.0) | no `shaderFloat64` | not faked: zink reports `doubles` itself and lowers fp64 with Mesa's soft-fp64 |

Not faked either, and checked in the guest's extension list: transform feedback 2/3 (wgvk's own fake of
`VK_EXT_transform_feedback`, `WGVK_NO_FAKE=1` removes it: Begin/End/BindBuffers are skipped, so no feedback data),
image load/store, SSBOs, compute, texture views, 16 UBOs, 24 SSBOs per stage.

**piglit**, the GL 1.x-3.3 selection of the T6 table (8964 tests, `scripts/piglit-wgvk.sh`; 15 chunks of 600, each
against a fresh server, `-j4`, about 6 minutes; then `scripts/piglit-rerun.sh` twice for what crashed):

| | pass | fail | crash | skip | timeout |
| --- | --- | --- | --- | --- | --- |
| vrend (virglrenderer on llvmpipe) | 8573 | 38 | 80 | 273 | 0 |
| vfront on zink on lavapipe (T6 table) | 8556 (8604 with the 48 OOM victims run alone) | 57 | 157 | 193 | 1 |
| **vfront on zink on wgvk** (webgpu 691cdb2 + this) | **7668** | 927 | 82 | 287 | 0 |
| (the same, `VFRONT_CAPS=honest`: GL 3.3) | 7672 | 863 | 82 | 347 | 0 |

The faked caps cost no passes (7668 against 7672): with GL 3.3 the 60 tests that need GL 4.0/4.1 skip, with the fakes they run and fail (the tessellation ones). So wgvk passes 89% of what zink on lavapipe does (936 fewer than 8604, 888 fewer than 8556). `piglit-wgvk-fails.txt`
lists the 909 tests that pass on lavapipe and do not on wgvk, with their status, group and first output line
(`tools/pgreport.py` writes it). The groups, with the wgvk gap behind each (one test of each run alone against a
fresh server, `scripts/pg-reps.sh`, shows the wgvk log line):

| tests | group | cause |
| --- | --- | --- |
| 221 | transform feedback (ext_transform_feedback, adjacency and GS tests that read feedback) | `VK_CMD_BEGIN/END_TRANSFORM_FEEDBACK_EXT`, `BIND_TRANSFORM_FEEDBACK_BUFFERS_EXT` "not implemented (T2), skipped": the extension is faked. some kill the server on `pipe_buffer_map_range` (`offset + length <= width0`, seen in `ext_transform_feedback-geometry-shaders-basic`) |
| 189 | integer textures and integer vertex attributes (texelfetch/texturesize with isampler/usampler, `vs-attrib-ivec/uvec`) | "None of the supported sample types (Sint) of RGBA32Sint match the expected (UnfilterableFloat)", and "Attribute base type (Sint for Sint32x4) does not match the shader's base type (Float)" |
| 78 | `tex-miplevel-selection` (`texture()` on 1D textures, glsl-1.20/1.30) and `tex-miplevel-selection-lod-bias` | probably 1D images being 2D in wgvk (the level chosen differs); no wgvk log line, not confirmed |
| 68 | multisampling: 2D multisample arrays, `arb_texture_multisample@texelfetch/texturesize`, `arb_sample_shading`, alpha-to-coverage | "multisampled image with 9 layers: WebGPU has one (layer 0 only)"; texture views past layer 0 of a 1-layer texture |
| 66 | shadow sampling with bias or gradients (`glsl-fs-shadow*-bias`, `shadow*grad`) | "[shadow-sample] ... WGSL has textureSampleCompare and ...Level(0) only": the fragment shader is not compiled |
| 51 | depth and stencil copies and blits, `copypixels`, `drawpixels` stencil, `copyteximage`, `depth-stencil-blit` | "Copy origin ... does not cover the entire subresource" (WebGPU copies whole depth/stencil subresources); "different front and back stencil references: WebGPU has one"; row pitches needing a compute copy |
| 50 | skip where lavapipe runs the test | sample counts above 4 (`MAX_INTEGER_SAMPLES`), image types WebGPU lacks |
| 43 | not attributed (one or two each: `vs-input-arrays`, `fs-execution-ordering`, `redeclare-pervertex-out-subset-gs`, uniform-block array indirect, texture buffer formats, ...) | in the list, group "other" |
| 37 | `varying-packing` | "render pipeline not made: [EntryPoint "main"] infringes limits" (WebGPU's inter-stage variable count) |
| 30 | formats and limits reported differently (`teximage-colors` luminance/snorm, `minmax` LOD bias range, texture buffer max size and formats, `maxuniformblocksize`, GS output components) | caps values and format emulation |
| 27 | tessellation | faked (above) |
| 18 | geometry shader parts (clip distance, primitive restart, line loops) | "a geometry shader draw without its parts (compile failed): skipped"; "buffer copy not in whole words" |
| 17 | `layered-rendering` clears of array and 3D layers | no wgvk log line; probes read the wrong layer |
| 5 | `arb_compute_shader` | "storage image of format X bound where the shader declares Y: a placeholder stands in" |
| 4 | gl_ClipDistance read by the fragment shader | "input slot VARYING_SLOT_CLIP_DIST not in WGSL" |
| 2 + 2 | server lost; logic ops (`gl-1.1-xor`, `gl-1.0-logicop`) | "logic ops are not in WebGPU (design: fake first)" |

Of the 157 crashes of the lavapipe run most were victims of its OOM kill; on wgvk 82 remain after two reruns
(`!opengl 1.1` 36, `ext_transform_feedback` 21, `arb_sample_shading` 12, `arb_framebuffer_object` 4): real
server aborts or tests that kill the server. These are in the same list when they passed on lavapipe (2 are).

**The server leaks.** A wgvk vfront-vtest passed 10 GB of resident memory in 1760 tests (the 12 GB VM was within
100 MB of an OOM kill), against lavapipe's run that finished in one 5 minute server. Not looked into (memfd
backings, Dawn/lavapipe allocations, textures zink keeps); `main-run.sh` restarts a server past `MAXRSS_MB`
(7000) and piglit-wgvk.sh gives each chunk its own. A test running when the server is restarted is reported
"lost connection to rendering server" (a crash); piglit-rerun.sh runs those again.

**Existing checks**: `tests/vfront.test` (glmark2 capture, 468 readbacks) still passes on wgvk: 468 of 468 images
within tolerance 8, 456 bit for bit equal to lavapipe's.

### In the machine (M1, 2026-10-10)

es2gears_wayland in the browser machine with RVGPU=1 on `?exec=webgpu`: the
guest's stock Mesa 25.0.7 virgl -> gpuserver -> ring -> rvgpu-worker.js ->
vfront -> zink -> wgvk -> Chrome's WebGPU (Metal, Apple M4 Max). gpuserver:
"the executor is here (1408 bytes of caps, 94 ms after our start)", then a
steady 60 submits/s, 240 records/s, 0.6-0.8 ms per wait; the gears drawn,
shown through stage A (Mesa's readback into wl_shm). Run from branch
`webgpu-e2e` (webgpu + gpu + the two hunks proposed to gpu: gpu-exec.js
`?exec=webgpu`, stage.sh staging rvgpu); hold script `build/e2e1.sh` there.

### The ring path

`vfront-ring` plays a capture as gpuserver does (ring v2 records, backings at
guest addresses) into rvgpu.c's consumer and vf_rvgpu.c, on one thread: glm-small
468/468 readbacks equal, none read before `ctx_done` passed its TRANSFER_GET
(`tests/vfront.test`, ring-vfront). The page Worker starts vfront on the ring
(`rvgpu-worker.js`, `vfront_rvgpu_start`); gpu-exec.js's `?exec=webgpu` (proposed
to the gpu branch) starts that Worker in place of vrend's.

### Known gaps

- 64-bit float vertex attributes (GL_DOUBLE through glVertexAttribPointer).
- Sample counts the driver lacks are rounded up.
- No video, no blob resources (virgl's VIRGL_CCMD_PIPE_RESOURCE_* video and
  blob paths), no SET_DEBUG_FLAGS; the page path has no zero-copy uploads.

## M2 bring-up: Blender 4.3.2 on zink on wgvk, natively (T2)

Measured 2026-10-10, branch `gpu-t2`, in the VM's arm64 trixie chroot (Debian's
Blender 4.3.2 and Mesa 25.0.7 zink, `MESA_GL_VERSION_OVERRIDE=4.3`) with wgvk
on Dawn native; scripts and how to run them in `tests/blender/`. The
reference is lavapipe in the same chroot. Default scene (the cube), frame 1,
1920x1080.

The chroot's own lavapipe (25.0.7) blends no SNORM formats, so Dawn gives it
no `texture-formats-tier1` and EEVEE's R16F, RG16Unorm, RG11B10 and RGB10A2
storage images cannot be made. Browsers have tier1 (design table), so Dawn
runs on the integration build's lavapipe (Mesa main, LLVM 20 copied into the
chroot: `chroot-wgvk.sh wgvk-main`), which gives tier1 and tier2; the timing
reference is that same lavapipe directly (`lvp-main`).

| what | result (wgvk vs lavapipe) | WebGPU passes | time (wgvk / lavapipe) |
| --- | --- | --- | --- |
| Workbench render (`-b -E BLENDER_WORKBENCH -f 1`) | 22 of 2073600 pixels differ by more than 8/255 (max 19); vs the spike's `wb-full-small.png`: 0.19% of pixels | 44 render passes -> 44 (0 splits), 25 compute passes; 25 draws, 17 dispatches; 33 buffer snapshots | render 1.28 s / 1.18 s |
| EEVEE render (`-b -E BLENDER_EEVEE_NEXT -f 1`, 64 samples) | 2934 pixels (0.14%) differ by more than 8/255 (max 54, faint lines on the cube); vs the spike's `eevee-p4-small.png`: 0.25% (lavapipe itself: 0.18%) | 535 render passes -> 599 (64 splits on usage scopes, one per sample), 1349 compute passes; 582 draws, 1931 dispatches; 2754 buffer snapshots (27 MB), 1 texture snapshot, 64 passes with a stand-in target, 67 non-finite clears | 26.0 s / 18.1 s; per sample 0.36 s / 0.17 s |
| EEVEE render passes (1 sample, `passes` mode, per-pass EXRs) | Combined, Depth, Normal, AO, Diffuse/Glossy colour and direct within 0.6% of pixels; Environment 19% and Position 16% differ (their storage writes run for depth-rejected fragments: WGSL has no early fragment tests, INTERFACES 6.1 proposal); Shadow 82% (see INTERFACES 6.2: indirect GS draws and `gl_ViewportIndex`) | | |
| UI startup, Workbench viewport (Xvfb) | not reached: zink on X11 needs an X11 surface (INTERFACES 9, T7) | | |

CPU side of wgvk in EEVEE: 1.46 s in vkQueueSubmit for 2513 draws and
dispatches (0.58 ms each, 0.50 s of it waiting on 10 pipeline compiles);
4258 bind groups made. Shader compiles (zink -> wgvk_compile -> Tint ->
LLVM) are what the first sample waits for.

Fixed in T2 to get here (commit "Blender EEVEE renders on zink on wgvk"):
passes without attachments, non-finite clear colours, compute passes that
sample what they write, 2D / 2D-array twin views and storage-format
placeholders (T1's files), image buffers for T3b's IMAGE_BUFFER, colour
write masks under EDS3 blend state (EEVEE's deferred passes wrote nothing).

Not hit by these scenes: vertex shaders that write storage (T3b/T4's
VS-as-compute), image atomics (EEVEE's shadow atlas is written by the
shadow pass's fragment shader, which the skipped GS draw never reaches),
texel buffers.

## T7: presenting (`vulkan/wgvk_present.c`, `vulkan/wgvk_wsi.c`, `vfront/vf_present.c`, `rvgpu`, `tools/www/rvgpu-worker.js`)

Measured 2026-10-10 in the test browser (Chrome for Testing, Metal adapter, M4 Max), Debian `daily` disk,
holds `build/h*.sh` of gpu-t7 (scripts in the worktree's `build/`, untracked). Guest-side client
`gpu/tools/present-test.py`: draws each frame on the GPU (a virgl CLEAR, then a white band over the top 32 rows
by TRANSFER_PUT2), 300x300, two buffers, each frame waits for its frame callback.

### Stage B (zero copy) in the page

A window whose buffer is a virgl resource (present-test `dmabuf`: the resource's backing handed over through
`zwp_linux_dmabuf_v1`, slot format 2) is drawn from zink's image of the resource: the Worker blits the
GPUTexture into its OffscreenCanvas and posts an ImageBitmap; nothing is read back.

| | `?exec=webgpu` (vfront → zink → wgvk) | `?exec=vrend` (WebGL2) |
| --- | --- | --- |
| frames shown | 592 in 20 s, 746 in 25 s with the page at 30 Hz (h1, h3); 818 drawn by the Worker in the 15 s run with the page at 60 Hz (h5) | 597 in 20 s at 30 Hz (h2) |
| Worker blit (texture → canvas → bitmap) | 0.08 ms average (30 Hz runs), 0.24 ms (h5; 6 ms max: the first pipeline) | 0.03 ms |
| page import (texImage2D of the bitmap) | 0.024-0.030 ms | 0.020-0.022 ms |
| bitmap posted → page's "imported" | 0.24 ms average (h5) | - |
| picture checked on the composed screen (probe.js) | band at the top, colour in the middle (h5) | band at the bottom before f45cc25 (vrend's flip), fixed there; not re-run yet |

The guest's frame rate is the display's: a frame callback fires when the page has drawn the frame, one
display frame after the commit (commit → callback p50 31.0-31.9 ms with the page at 30 Hz, h1/h3; the same for
stage A and for vrend). Some holds ran the page at 30 Hz (machine load), others at 60.

### Stage A (Mesa's own present: readback into wl_shm), the fallback for clients without dmabuf

Served by vfront's TRANSFER_GET (T6). present-test `shm`: 0 of 76 / 94 checked frames read back wrong
(colour and band), GPU side per frame (submit + wait + readback + copy, guest clock) p50 2.7-2.8 ms on webgpu,
2.0 ms on vrend. es2gears_wayland draws (the screenshot h1-gears) at the display's rate.

### glmark2-es2-wayland (stage A, windowed)

| scenes | webgpu | vrend | llvmpipe in the guest |
| --- | --- | --- | --- |
| 640x480 build, texture, phong, refract (page at 30 Hz) | 120, 118, 116, 98: **112** (h1) | 120, 119, 120, 98: **113** (h2) | 1, 29, 8, 1: **8** (h2) |
| 800x600 terrain, jellyfish, effect2d, pulsar, ideas, refract (60 Hz) | 129, 226, 224, 215, 106, 122: **169** (h5) | (h6, pending) | - |

Windowed glmark2 is bound by the window system (a wl_shm buffer comes back once the page has uploaded it),
not by the executor: both executors stop at the same rate. `--off-screen` would measure the executor, but
it loses the GPU server at once on both executors: gpuserver runs out of file descriptors (`no backing of
8 bytes ... error 24`: every fence is a resource with a backing file) — the gpu path's to fix.

### Natively (`tests/wsi.test`)

wgvk's own WSI: a headless surface and swapchain, 9 frames cleared and presented, every image read back with
its last frame's colour. Venus' host checks: memory exportable as dma-buf with the LINEAR modifier as a
colour attachment, a dedicated exportable LINEAR-modifier image, sync-fd semaphores importable/exportable,
fd -1 imported and waited on: 7 ok. With T5's vkr (scratch merge gpu-t7 + gpu-t5, native-venus):
Debian's venus in the chroot now offers `VK_KHR_swapchain` on wgvk (`vulkaninfo`; T5's build without T7:
none).

Window systems natively (`native.sh` now builds x11,wayland; `wsi.test` runs them when the VM has the tools):
vkcube on Xvfb (xcb, 100 frames, the grey clear colour on the screen) and vkcube-wayland on weston's headless
backend (a screenshot shows the textured cube); in the trixie chroot, zink 25.0.7 through kopper on wgvk:
`glxinfo -B` (zink Vulkan 1.1 on wgvk, GL 3.3), es2gears_x11 ~800-1560 frames/s and vkgears ~1650 frames/s
under Xvfb, gears on the screen. glxgears stops in a geometry shader compile (`VARYING_SLOT_PRIMITIVE_ID`),
Blender's UI on a depth/stencil copy assertion: not presenting problems.

### Not done

- Venus WSI images in a window **in the browser** (VK-07, 2026-10-12): the hook was already in `webgpu`
  (e3130c5) but never reached a window, because vkr refused every non-host-visible blob and Mesa's WSI
  only allocates device-local window images; fixed in `virglrenderer-page.diff`. Natively proven:
  `vkr/scripts/venus-wsi.sh` (vkr-vtest + the chroot's `vkcube --wsi wayland` on weston headless GL):
  100 frames, screenshot shows the textured cube. The page's own drawing path (Worker, `memory_resolve`)
  is unchanged and has not been run in a browser since (VK-09 does it in the machine).
- GL zero copy for stock Mesa: needs the emulated render node (gpu path); stock virpipe presents via wl_shm.
- Window alpha: the Worker's canvas is opaque.

## T5: venus (vkr on wgvk)

Measured 2026-10-10. Guest side: Debian trixie's Mesa 25.0.7 venus
(`libvulkan_virtio.so`, `VN_DEBUG=vtest`). Host side: virglrenderer main
c8e3fc3's vkr + `vkr/virglrenderer-page.diff`, on wgvk (T1 + T2 merged for
the runs; T2 was not in `webgpu` yet: branch `gpu-t5-int` = gpu-t5 + gpu-t2 +
gpu-t5-server, test-only).

### Natively (`gpu/webgpu/vkr/scripts/`, build VM)

`vkr-vtest` is gpuserver's venus half and the page's executor in one process
(ring v3 records, rvgpu.c, rvgpu_vkr.c, vkr, wgvk on Dawn native on lavapipe;
guest memory a reserved range, blobs memfds mapped into it).

| test | result |
| --- | --- |
| `native-venus.sh 'vulkaninfo --summary'` (arm64 client in the trixie chroot) | `Virtio-GPU Venus (wgvk (WebGPU: llvmpipe ...))`, Vulkan 1.1 |
| `t2-venus.sh` (T2's t2-test through venus vs lavapipe directly) | **22/22 outputs equal** (clear, triangle, indexed, instanced, indirect, indirect-gpu, fan-last, strip-last, depth, ubo-dyn, texture, compute, compute-indirect, copies x3, clear-att, flip-y, split x2, selfdep x2) |
| `bench.sh 10000 3` (vkr-bench: 10000 x push constants + dispatch, the word checked) | correct on lavapipe, wgvk and venus; **vkr + wgvk: 60 406 venus commands (394 ring commands + 60 012 inside executed command streams) in 65 ms = 1.08 µs per command** (decode, wgvk recording, and the replay at submit); guest recording 0.05-0.10 µs per command pair (arm64 native) |
| empty submit -> fence (bench.sh) | wgvk 1.18 ms, venus 1.98 ms (the ring is polled every 1 ms while busy) |
| `rv-venus.sh .../vulkaninfo --summary` (Debian riscv64 venus under native rv, interpreter) | same device; vkr side 163 commands in 15 ms |

Since T7's merge, vulkaninfo through venus lists VK_KHR_swapchain, VK_EXT_external_memory_dma_buf and
VK_EXT_image_drm_format_modifier; `t2-venus.sh` on the merged tree: **38/38 equal** (adds geometry shaders,
queries, the buffer heap, depth copies).

### In the browser (four test-browser holds before holds stopped; the rest is the manual test sheets)

- `vulkaninfo --summary` (Debian riscv64 venus in the machine -> gpuserver -> ring v3 -> vkr in the page ->
  wgvk on Chrome's WebGPU): `Virtio-GPU Venus (wgvk (WebGPU: device))`, Vulkan 1.1, in 0.4-2.6 s.
- vkr-bench: fence never seen by the guest. Found and fixed (not yet re-run in the browser):
  rvgpu.wasm lacked `rvgpu_guest_upload_buffer/_upload_texture/_from_mapped/_ptr` (wgvk's weak references
  do not pull JavaScript library functions in: GUEST windows were never uploaded or downloaded); T2's retire
  called `getMappedRange` twice on the same range for GUEST windows (throws in the page); `wgvk_wait`
  re-waited on completed futures (emdawnwebgpu's timed WaitAny then loops on microtasks and starves the
  Worker; T0/T7 had fixed the same in `webgpu` meanwhile).

vkcube: not run. Venus' WSI needs a dma-buf-capable renderer
(`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`, no
software WSI path in venus); with Xvfb (no DRI3) even lavapipe through stock
virglrenderer refuses (`No DRI3 support detected`). Presenting is T7's.

## VK-16: Blender's UI on the real executor (wgvk fixes)

Blender 4.3.2 (Debian riscv64) in the machine on `?exec=webgpu` (tools/gpu-nm.sh `blender`, `blender-bg`) drew its window
at first with all text and every overlay missing, and the executor's ring drain died once. Native repro: `blender-vfront.sh ui`
(tests/blender, a vfront-vtest build and a wgvk from this tree) shows the same, and each fix below is checked there.
- **NaN depthClearValue** killed the ring drain ("Floating-point value (nan) is not finite" in beginRenderPass): webgpu.h's
  undefined depth clear value is NaN and emdawnwebgpu hands it to the browser as it is. Every render pass built from
  `WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT` (render, copies, blits) now starts at 0.
- **Integer vertex formats read as float inputs** (zink gives Blender's text and overlay attributes a 32-bit SINT/UINT format and
  the shader reads the bits as floats; the pipeline was refused: "Attribute base type (Sint ...) does not match the shader's base
  type (Float)"): those bindings are pulled (shader-side fetch, bits kept; `wgvk_shader.vs_float_inputs`). A numeric
  conversion (i2f) was tried first and is wrong: the overlays came out as scattered lines.
- **Colour outputs of another type than the attachment** (a float output into R16UI, Blender's outline ids; refused likewise):
  the fragment variant declares the output as the attachment's type, bits kept (`wgvk_fs_key.out_type`,
  `wgvk_shader.fs_float_outputs`).
- **Buffer/image copies with rows that are not whole words** (a 270-byte row of 8-bit texels: Blender's glyph atlas) were dropped
  ("T2 needs a compute copy"): one copy per row, which WebGPU allows with no bytesPerRow (single-block-wide formats).
- **Buffer copies of a size that is not a whole word** (34 bytes) were dropped: the whole words by a copy, the last 1-3 bytes by a
  one-invocation compute shader that keeps the destination's other bytes (`wgvk_meta_tail_copy_pipeline`).
  Both copies are in `t2-copies` (equal to lavapipe's).
- Left: "primitive restart in a GPU-side index rewrite: ignored (T2)" (seen once in the UI), rows of blocks of compressed formats
  with unaligned rows, copies at offsets that are not words.
Blender in the machine: tools/gpu-nm.sh `blender-bg` (Workbench 480x270 vs llvmpipe's: 0 of 129600 px more than 8/255 off, max 7;
Blender's own render time 13.8 s on the executor, 2:05 on llvmpipe) and `blender` (window: UI text and the default scene's viewport,
no GL error in its log, no pipeline refused). Blender does not end after "Blender quit" in this machine, with the GPU or without
(rv side, not the GPU's): the harness kills it.
