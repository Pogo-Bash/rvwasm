# GPU on WebGPU: the interfaces between the tracks

Owned by T0 (branch `webgpu`). Every interface that crosses a track
boundary is here, with its owner. The owner decides; others propose a
change in their branch (edit this file, say so in the report) and the
orchestrator merges. Design: `docs/gpu-webgpu-design.md`. Status words:
**stub** = a header or empty function is in the tree and compiles, the
behaviour is not there; **proposed** = text only, the owner fills it in;
**agreed** = in use by more than one track.

Version 2, 2026-10-10 (v1: a0bf5ab).

## 0. Where the code lives (T0)

```
gpu/webgpu/
  INTERFACES.md              this file
  build/                     T0: one command per build, see section 10
  mesa/src/webgpu/           OUR Mesa code. The build links this directory into
                             the pinned Mesa tree as src/webgpu (a symlink), so
                             it is edited and merged in this repo like any file:
    meson.build              T0
    include/wgvk.h           T0: the public entry points (section 1)
    vulkan/                  T1 (objects) + T2 (commands): wgvk_*.c, wgvk_*.h
                             (T0's skeleton is there now: replace it)
    compiler/                T3: NIR legalisation passes + NIR -> WGSL
    poly/                    T4: libpoly glue
    vfront/                  T6: virgl decoder on zink's pipe_context
    rvgpu/                   T0: the page host: rvgpu.h, rvgpu.c (ring),
                             rvgpu_page.c (device, smoke), rvgpu_native.c
                             (guest memory for native tests), library_rvgpu.js,
                             meson.build (links rvgpu.js/rvgpu.wasm)
    tests/                   C sources of tests (smoke-clear.c, ring-test.c)
  vkr/                       T5: virglrenderer pin + patch + glue
  tests/                     *.test scripts that build/check.sh runs (section 10)
  profiles/                  T0: the wgmask P4 profile as an oracle
patches/mesa-page.diff       T0 (others propose hunks): changes to files that
                             exist in Mesa (meson hooks, Emscripten fixes).
                             New files never go in the diff; they live in
                             gpu/webgpu/mesa/src/webgpu/.
tools/www/rvgpu-worker.js    T0: the page Worker host (section 8)
tools/www/rvgpu-smoke.html   T0: the page smoke test
```

The pinned Mesa commit is in `gpu/webgpu/build/mesa.pin` (main
35b085c4c067, 2026-10-09, 26.3.0-devel), Dawn's in `build/dawn.pin`
(c943ea32, for Dawn native and emdawnwebgpu alike). The guest's Mesa is
never rebuilt.

## 1. Creating wgvk on a given device (T0) - stub

`gpu/webgpu/mesa/src/webgpu/include/wgvk.h`:

```c
/* Hand wgvk the one WebGPU device it runs on. In the page the Worker made it
 * (navigator.gpu, then WebGPU.importJsDevice); natively a test made it on
 * Dawn native. Call once, before the first vkCreateInstance. wgvk takes a
 * reference on each object. */
void wgvk_use_device(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device);

/* The loader-free way in, for consumers built into the same module (zink in
 * vfront, vkr): wgvk_use_device + vkCreateInstance. Returns the instance's
 * vkGetInstanceProcAddr in *gipa. */
VkResult wgvk_create_instance(WGPUDevice device, const VkInstanceCreateInfo *info,
                              const VkAllocationCallbacks *alloc, VkInstance *instance,
                              PFN_vkGetInstanceProcAddr *gipa);
PFN_vkVoidFunction wgvk_GetInstanceProcAddr(VkInstance instance, const char *name);
```

As a native ICD (`libvulkan_webgpu.so` +
`$B/native/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json`, loaded by the
Vulkan loader for vulkaninfo, vkcube, dEQP, zink), nothing called
`wgvk_use_device`. wgvk then makes its own Dawn native instance, adapter and
device on first `vkEnumeratePhysicalDevices`. Two things make that work
(measured: without them it deadlocks or recurses):

- The loader holds its lock while it calls the ICD. So wgvk links Dawn from
  `$W/dawn-run`, which also holds a *copy* of the system loader
  (`build/dawn-pc.sh`); Dawn opens libvulkan.so.1 from its own directory
  first, gets a second, independent loader, and no lock is taken twice.
- That second loader finds wgvk again. While wgvk is making its Dawn device,
  its own `vkEnumeratePhysicalDevices` returns 0 devices.

`WGVK_DAWN_DRIVER_FILES` (a VK_DRIVER_FILES value) names the drivers Dawn
may use, so the app's `VK_DRIVER_FILES` can name wgvk alone:
`VK_DRIVER_FILES=$WGVK_ICD WGVK_DAWN_DRIVER_FILES=$LVP_ICD vulkaninfo`.
`WGVK_BACKEND=null` puts Dawn on its null backend. Debugging helper:
`WGPUTexture wgvk_image_texture(VkImage)` (the GPUTexture of an image;
T7 will want it).

One physical device, one queue family with one queue
(graphics | compute | transfer).

## 2. Threads and waiting (T0) - agreed by design

- Every WebGPU call happens on one thread: the Worker's own thread in the
  page, the test's main thread natively. WebGPU objects cannot cross
  threads in the browser.
- wgvk sets `VK_QUEUE_SUBMIT_MODE_IMMEDIATE` and starts no threads.
  Front ends must not call wgvk from their own threads: zink without its
  flush thread, vkr without its ring thread in the page (T5 and T6 own
  how).
- Blocking (`vkWaitForFences`, `vkWaitSemaphores`, `vkQueueWaitIdle`,
  `vkDeviceWaitIdle`, `vkGetQueryPoolResults` with WAIT, mapping memory
  the GPU wrote) goes through one function:

  ```c
  /* wgvk/vulkan: wait for WebGPU futures (onSubmittedWorkDone, mapAsync).
   * Natively Dawn blocks. In the page rvgpu.wasm is built with JSPI, and
   * wgpuInstanceWaitAny suspends the wasm stack until the promise settles;
   * that only works below an export listed in rvgpu_jspi_exports
   * (rvgpu/meson.build: rvgpu_ring_drain, rvgpu_smoke_clear), which then
   * returns a Promise to JavaScript. */
  VkResult wgvk_wait(struct wgvk_device *dev, const WGPUFuture *f, unsigned n, uint64_t timeout_ns);
  ```

- Nothing else may block: no `Atomics.wait` on the Worker's thread while
  WebGPU work is pending (its callbacks run from the event loop).

## 3. Guest memory and the ring (T0) - stub

The machine's memory is a separate `WebAssembly.Memory` (memory64, shared,
up to 8 GiB) from rvgpu.wasm's own (wasm32). C code cannot point into it.
"Guest address" below is an offset in the machine's memory (a `uint64_t`).
`gpu/webgpu/rvgpu/rvgpu.h`:

```c
/* Attach to gpuserver's ring (PROTOCOL.md, ring v2) at guest address `ring`.
 * The page first gave the machine's Memory to library_rvgpu.js
 * (rvgpu-worker.js does). Returns 0, or -1 when the magic/version do not match. */
int rvgpu_ring_attach(uint64_t ring, uint32_t ring_size, uint64_t caps, uint32_t caps_size);
/* Consume records up to the ring's head, dispatching each to the front end that
 * registered its op (section 6). Returns the records handled. A JSPI export. */
int rvgpu_ring_drain(void);
typedef void (*rvgpu_op_fn)(void *front, const struct rvgpu_record *rec, const void *payload);
void rvgpu_ring_register(uint32_t op, rvgpu_op_fn fn, void *front);
/* Completion (in order per context): raise ctx_done[ctx] and done to serial. */
void rvgpu_complete(uint32_t ctx, uint32_t serial);

/* Guest memory. In the page these go to the machine's SharedArrayBuffer through
 * JavaScript; natively (tests) to a host block given by rvgpu_guest_set_host(). */
uint32_t rvgpu_guest_load32(uint64_t addr);                 /* Atomics.load */
void rvgpu_guest_store32(uint64_t addr, uint32_t v);        /* Atomics.store + notify */
void rvgpu_guest_read(void *dst, uint64_t addr, size_t len);
void rvgpu_guest_write(uint64_t addr, const void *src, size_t len);
/* Straight from the guest's pages to the GPU, no copy through the module heap
 * (queue.writeBuffer / writeTexture take a view over the shared memory). */
void rvgpu_guest_upload_buffer(WGPUQueue q, WGPUBuffer b, uint64_t boff, uint64_t addr, size_t len);
void rvgpu_guest_upload_texture(WGPUQueue q, const WGPUTexelCopyTextureInfo *dst, uint64_t addr,
                                const WGPUTexelCopyBufferLayout *layout, const WGPUExtent3D *size);
/* From a mapped MAP_READ buffer (after mapAsync) into the guest's pages. */
void rvgpu_guest_from_mapped(WGPUBuffer mapped, uint64_t boff, uint64_t addr, size_t len);
/* Natively only (tests): the host pointer of a guest range; NULL in the page. */
void *rvgpu_guest_ptr(uint64_t addr, size_t len);
void rvgpu_guest_set_host(void *base, uint64_t size);
```

The header is `mesa/src/webgpu/rvgpu/rvgpu.h` (also: `rvgpu_init()`, the
page's device into wgvk; `rvgpu_set_state`, `rvgpu_set_caps`;
`rvgpu_present_lookup`). In the page the guest functions are in
`rvgpu/library_rvgpu.js`: the Worker sets `Module.rvgpuMemory` (the
machine's `WebAssembly.Memory`, or a SharedArrayBuffer for tests) and
`Module.rvgpuDevice` when it instantiates the module;
`Module.rvgpuJsObject(ptr)` gives the JavaScript object of a webgpu.h handle.
Natively they are in `rvgpu/rvgpu_native.c` over `rvgpu_guest_set_host()`.
The ring consumer is tested natively by `tests/ring.test`.

## 4. wgvk objects (T1) - implemented (T1, branch gpu-t1)

### 4.1 Memory and buffer objects (`vulkan/wgvk_bo.h`, `wgvk_memory.c`)

```c
enum wgvk_window_kind { WGVK_WINDOW_NONE, WGVK_WINDOW_HOST, WGVK_WINDOW_GUEST };
struct wgvk_window { enum wgvk_window_kind kind; union { void *host; uint64_t guest; }; };
struct wgvk_bo {
   WGPUBuffer buffer;      /* NULL until a buffer is bound (wgvk_memory_buffer) */
   uint64_t offset, size;  /* the range of `buffer` this bo is (size 4-aligned) */
   struct wgvk_window window;
   uint32_t flags;         /* WGVK_BO_HOST_VISIBLE, WGVK_BO_GPU_WRITTEN */
};
/* the memory sync points, called by T2's replay */
void wgvk_bo_upload(struct wgvk_device *, const struct wgvk_bo *, uint64_t offset, uint64_t size);
void wgvk_bo_download(struct wgvk_device *, const struct wgvk_bo *, uint64_t offset, uint64_t size,
                      WGPUBuffer mapped, uint64_t mapped_offset);   /* from a mapped MAP_READ buffer */
void *wgvk_bo_map(const struct wgvk_bo *);   /* CPU address of the window, NULL for GUEST in the page */
struct wgvk_bo wgvk_buffer_bo(const struct wgvk_buffer *, uint64_t offset, uint64_t size);
```

- Memory types: 0 = DEVICE_LOCAL (GPU only), 1 = HOST_VISIBLE |
  HOST_COHERENT | HOST_CACHED with a HOST window (malloc'd by wgvk), or a
  GUEST window set by T5 for an imported venus blob (`BLOB_CREATE`).
  Heaps are nominal (4 GiB / 2 GiB; WebGPU says nothing).
- A VkDeviceMemory is one GPUBuffer with every buffer usage, made on the
  first vkBindBufferMemory: memory only images use never gets one. A
  VkBuffer is a range of it; alignment = max(minUniform/Storage
  BufferOffsetAlignment) (256 on the Mac).
- Optimal images need DEVICE_LOCAL memory only and keep nothing in it.
  Linear images (copies only) may be in host-visible memory; their rows are
  256-byte aligned (`wgvk_image.level[l].row_pitch`) so T2 copies
  memory ↔ texture in one copy each way.
- `wgvk_bo_upload` takes the window's bytes into the GPUBuffer
  (`queue.writeBuffer`, `rvgpu_guest_upload_buffer` for GUEST);
  `wgvk_bo_download` puts a mapped staging copy back into the window.

### 4.2 Binding remap (T1 owns the layout, T3 the shader pass) - implemented (T1)

Header `mesa/src/webgpu/include/wgvk_remap.h` (the struct; `wgvk_compile.h`
includes it), numbering in `vulkan/wgvk_remap.c` (`wgvk_remap_assign_set`,
`wgvk_remap_find`; the compiler's `wgsl-corpus -layout` links the same file).
Vulkan (set, binding, array element) → WebGPU (group, binding):

- set `s` → group `s` (`maxBoundDescriptorSets` = 4, so sets 0..3).
- Within a set layout, Vulkan bindings in increasing binding order take
  consecutive WebGPU bindings from 0, one per array element. A combined
  image sampler takes `2·count`: the textures first, then the samplers.
  An inline uniform block takes one (a uniform buffer the set owns).
  Bindings with `descriptorCount` 0 take none and have no entry. Texel
  buffers are storage buffers in WebGPU (the shader decodes the format,
  T3b); input attachments are textures.
- Dynamic UBOs and SSBOs keep their place (`WGVK_REMAP_DYNAMIC`) and use
  WebGPU dynamic offsets; WebGPU orders those by binding number, which is
  Vulkan's order here.
- A set may use at most 900 WebGPU bindings
  (`vkGetDescriptorSetLayoutSupport` says no beyond that).
- Driver bindings (push constants beyond the immediates, draw parameters,
  buffer heap table and heap buffers, poly scratch) are
  `WGVK_DRIVER_BINDING_BASE` (900)..999 of `driver_group`: **its own group
  right after the app's sets while one is free** (the app uses sets 0..k-1:
  group k), else group 3, shared with the app's set 3. The fixed slots are
  `WGVK_DRIVER_BINDING_*` in the header (proposed).
- Push constants: `push_constant_bytes` = end of the last range;
  ≤ `immediate_bytes` (the device's `maxImmediateSize`, 64) are immediates,
  more go to the driver UBO at 900.
- The table (T3's pass reads it; `binding` was T3's proposal, `flags`,
  `stages` and `immediate_bytes` are new, `push_constant_bytes` is 16 bits
  for 256-byte push constants):

  ```c
  struct wgvk_binding_remap {          /* one per (set, binding) with descriptors, ordered by (set, binding) */
     uint8_t set, group;
     uint16_t binding;                 /* the Vulkan binding number */
     uint16_t wg_binding;              /* first WebGPU binding: element i is wg_binding + i */
     uint16_t wg_sampler_binding;      /* combined image samplers: the sampler of element i is this + i */
     uint16_t count;                   /* array elements (an inline uniform block: 1) */
     uint8_t flags;                    /* WGVK_REMAP_DYNAMIC, WGVK_REMAP_HEAP (4.4, not yet) */
     VkDescriptorType type;
     VkShaderStageFlags stages;
  };
  struct wgvk_layout_remap {
     const struct wgvk_binding_remap *b;  unsigned n;
     uint8_t driver_group;
     uint16_t push_constant_bytes, immediate_bytes;
  };
  ```

- Where it comes from (T2 passes it as `wgvk_shader_key.layout`):
  `wgvk_pipeline_layout.remap` (vkCreatePipelineLayout), or
  `wgvk_layout_remap_build(pdev, set_layouts, n, push_bytes, &out)` for the
  runtime's `vk_shader` path, which has set layouts and push constant
  ranges but no pipeline layout.
- Checked: `tests/objects.test` (objects-remap: a two-set layout against the
  numbering above); `tests/compiler.test` runs Blender's 177 modules with
  `-layout` (each module's own descriptors numbered by this code): 169 emit,
  all valid in Tint (8 blocked by T3 tags, none by the remap).

### 4.3 What T1 gives T2 (`vulkan/wgvk_private.h`)

Mesa runtime objects subclassed as `wgvk_*`:

- `wgvk_physical_device`: `limits` (the WebGPU device's), `wf` (its
  features), `compile_caps` (for `wgvk_compile`; also
  `wgvk_device_compile_caps`), `formats[]` (4.5).
- `wgvk_image`: `tex`, `fmt`, `wformat`, `wdim` (1D images are 2D),
  `wg_layers`, `wusage` (every usage the format allows that wgvk may need:
  copies, sampling, render attachment where renderable, storage when asked
  and possible), `mem`/`mem_offset`, `level[]` (linear layout).
- `wgvk_image_view`: one WebGPU view per use: `sampled` (TextureBinding,
  with the component swizzle of format + view), `storage` (one level, cubes
  as 2D arrays), `attachment` (one level and layer; a 3D image: the 3D view
  and `depth_slice`); NULL when the use is impossible. `aspect` for one
  aspect of a depth/stencil image.
- Views in a format WebGPU cannot reinterpret the texture as (anything but
  sRGB ↔ not, e.g. R32_UINT of RGBA8, zink's storage views): the image gets
  a texture per such format (`wgvk_image.alias[]`, same size, made with the
  first such view) and the view's WebGPU views are of it (`view->alias`,
  -1 = the image's own). `wgvk_image.current` says which texture has the
  latest texels. **T2:** before a command reads or writes the image through
  a view, call `wgvk_image_make_current(dev, enc, img, view->alias)`; before
  copies, clears, blits or attachments of the image's own format,
  `wgvk_image_make_current(dev, enc, img, -1)` (encoder level, outside a
  pass; it copies the whole image through a staging buffer when the owner
  changes, nothing otherwise). Not possible (`view->emulated`, logged once):
  depth/stencil, multisampled, or another block size (compressed ↔
  uncompressed views).
- `wgvk_sampler`: `sampler`, `nonfiltering` twin
  (`wgvk_sampler_nonfiltering`), `comparison`, `emulate`
  (`WGVK_SAMPLER_BORDER`, `MIRROR_CLAMP`, `LOD_BIAS`, `UNNORMALIZED`,
  `REDUCTION`: what the shader must do, a variant key for T3b).
- `wgvk_buffer_view`: the texel format (`fmt`) T3b decodes.
- `wgvk_descriptor_set_layout`: its remap (4.2), per-stage counts,
  inline-block offsets, immutable samplers.
- `wgvk_descriptor_set`: `desc[]` indexed by WebGPU binding (kind, object
  pointers, object id), `generation`, inline block storage and its buffer.
  `wgvk_descriptor_set_create/_write/_write_template/_destroy` serve push
  descriptors too.
- `wgvk_pipeline_layout`: `remap` (= `wgvk_shader_key.layout`).
- Bind groups (agreeing with T3b's 6.1): `wgvk_bgl_entries_from_shaders`
  builds a group's layout entries from the compiled shaders (only what a
  stage uses; visibility merged; dynamic offsets from the remap);
  `wgvk_bgl_get` caches layouts by content; `wgvk_bind_group_get(dev, set,
  bgl, driver_entries, n)` makes a set's bind group for a layout (per-set
  cache, then a device cache by content; dummies for unwritten descriptors;
  non-filtering sampler twins; driver entries at 900+).
- Syncs (4.6) and every object carry an `id` that is never reused (cache
  keys; WebGPU handles are reused addresses).

**Overlap with T2 (gpu-t2 at 92f1535).** T2 wrote stand-ins
(`wgvk_objstub.c`: views, samplers, set layouts, sets, compile caps), its
own bind group code (per-set bind groups with snapshot rebuilds, in
wgvk_cmd.c) and its own sync type and submission list. Proposed split when
the two meet: T2 keeps bind group *construction* (it is tied to the replay's
usage-scope snapshots) but takes the entries from T1's descriptors
(`desc[]`, dummies, non-filtering twins, inline blocks, texel views) and
T1's `wgvk_bgl_entries_from_shaders`; `wgvk_objstub.c` goes; T2 keeps the
submission list and T1's `wgvk_sync` gives way to T2's, with T1's
`on_value` hook (completion words for T5) moved into it. Names that
collide today: `wgvk_bgl_get`, `struct wgvk_bgl`, `wgvk_descriptor_set_*`,
`struct wgvk_descriptor`, `wgvk_sync_type`, `struct wgvk_submission`.

### 4.4 The buffer heap: the driver side (`wgvk_heap.c`)

T3b lowers a stage's storage buffers past `caps.max_storage_buffers` to the
heap (6.1). T1 fills it per draw/dispatch for T2:

```c
#define WGVK_HEAP_ARENAS 2      /* caps.heap_arenas: driver bindings 910, 911 */
struct wgvk_heap_arena { WGPUBuffer buffer; uint64_t offset, size; };
int wgvk_heap_build(struct wgvk_device *, const struct wgvk_descriptor *const *slots, unsigned n,
                    uint32_t *table /* 4 words per slot: binding 902 */,
                    struct wgvk_heap_arena arenas[WGVK_HEAP_ARENAS]);
```

- Amendment to 6.1's "allocate storage memory from a few large arenas": an
  arena is the GPUBuffer of the memory object the slot's VkBuffer is in,
  bound over the window covering the slots (offset aligned down to
  minStorageBufferOffsetAlignment). No suballocation of all memory into
  shared GPUBuffers: that would put every buffer in the same WebGPU usage
  scope as any writable heap binding (WebGPU tracks usage per buffer, not
  per range) and make T2 snapshot far more. zink already suballocates
  its buffers from large memory objects, so a stage's SSBOs are normally
  in one or two. More than `WGVK_HEAP_ARENAS` memory objects in one
  stage's heap slots: the rest read 0 (logged once); if that happens, the
  next step is arena suballocation for storage-usage buffers only.
- Unwritten slots get {0, 0, 0, 0}; unused arena bindings a dummy buffer.
- Checked: `objects-internal-heap` (3 SSBOs in 2 memory objects + an
  unwritten slot). Natively Dawn on lavapipe allows 16 storage buffers per
  stage; the Mac 10.

### 4.5 Formats (`wgvk_format.h`, `wgvk_format.c`)

`wgvk_format_get(pdev, VkFormat)` → `{wf, vf, flags, caps, swizzle[4],
block_bytes/w/h, optimal/linear/buffer features}`. WebGPU's per-format
capabilities follow Dawn's Format.cpp rules with the device's features
(core, texture-formats-tier1/2, float32-filterable/-blendable,
rg11b10ufloat-renderable, bgra8unorm-storage, depth32float-stencil8,
BC/ETC2/ASTC, unorm16). Flags T2/T3b act on:

- `WGVK_FMT_SWIZZLED`: stored as another WebGPU format read through a
  swizzle (B8G8R8A8 integer/snorm, A2R10G10B10, BC1 RGB with alpha one);
  not renderable or storage.
- `WGVK_FMT_DEPTH_NO_COPY` (depth24plus*: no depth copies at all),
  `WGVK_FMT_DEPTH_NO_COPY_DST` (depth32float*: not a copy destination):
  T2 copies through a shader.
- `WGVK_FMT_SUBSTITUTE`: D32S8 stored as depth24plus-stencil8 when the
  device lacks depth32float-stencil8.
- `WGVK_FMT_VERTEX_PULL`: a vertex format WebGPU lacks (the 2_10_10_10 ones
  other than unorm/snorm ABGR): fetched by the shader (T3b vertex pulling).
- Texel buffers: 8/16/32-bit plain colour formats and 10:10:10:2, as
  planned emulation (T3b decodes). R32 storage images advertise atomics
  (T3b's IMAGE_BUFFER lowering).
- Depth formats advertise linear filtering (the shader filters, T3b);
  WebGPU filters only comparisons.

### 4.6 Syncs, queries (`wgvk_sync.c`)

One sync type for fences and binary/timeline semaphores: a value the CPU
sees plus signals pending on queue serials (`wgvk_queue_next_serial`,
`wgvk_queue_track(serial, future, complete_fn)`, `wgvk_sync_signal_at`,
`wgvk_queue_wait_serial`); waiting goes through `wgvk_wait` only.
`wgvk_sync.on_value(data, value)` is called whenever a value moves: T5
points it at a sync's word in shared memory. (See the T2 overlap above.)
Queries: not yet.

## 5. Commands and pipelines (T2) - implemented (natively; the page's draw smoke test passes in Chrome)

Code: `vulkan/wgvk_cmd.c` (recording, the replay loop, bind groups, dispatch,
host-visible memory traffic, submission), `wgvk_render.c` (render passes,
draws, usage scopes), `wgvk_copy.c` (transfers, clears, blits, resolves),
`wgvk_meta.c` (driver WGSL pipelines), `wgvk_pipeline.c` (key, cache,
async creation), `wgvk_shader.c` (Mesa's common pipelines on
`wgvk_compile`), `wgvk_sync.c` (completion), `wgvk_replay.h`. Tests:
`tests/t2.test` (t2-test: 21 outputs compared with lavapipe; smoke-draw).

**Recording and replay.** Commands are recorded by Mesa's `vk_cmd_queue`;
the commands with two forms go through Mesa's translators at record time
(legacy render passes become dynamic rendering, `vkCmdCopyBuffer` becomes
`vkCmdCopyBuffer2`, secondaries are inlined), so the queue holds one form.
At `vkQueueSubmit` every command buffer of the submission is replayed into
one GPUCommandEncoder. State commands are replayed through Mesa's common
implementations (`vk_command_buffer::dynamic_graphics_state`;
`vkCmdBindPipeline` reaches `vk_device_shader_ops::cmd_bind_shaders`).

**Pipelines.** wgvk uses Mesa's common `VkPipeline`, pipeline cache and
shader objects: a `vk_shader` is T3's compiled WGSL (+ the NIR of vertex
shaders, for variants). The GPURenderPipeline is made per draw state:

```c
/* Everything WebGPU bakes into a render pipeline (wgvk_private.h). Zeroed, then filled; hashed with XXH64,
 * compared with memcmp. */
struct wgvk_pipeline_key {
   uint64_t vs, fs;                    /* WGSL hashes of the variants drawn with */
   uint64_t layout;                    /* what the bind group layouts depend on beyond the WGSL (dynamic offsets) */
   uint8_t topology, strip_index_format, front_face, cull_mode;
   uint8_t unclipped_depth, sample_count, alpha_to_coverage, color_count;
   uint32_t sample_mask;
   uint8_t vb_count, attr_count, pad[2];
   struct { uint32_t stride; uint8_t step_mode, binding, pad[2]; } vb[8];   /* binding: Vulkan's */
   struct { uint32_t offset; uint8_t format, location, slot, pad; } attr[16];
   struct { uint32_t format; uint8_t write_mask, blend_enable, color_op, color_src, color_dst,
            alpha_op, alpha_src, alpha_dst; } color[8];
   struct { uint32_t format; uint8_t depth_write, depth_compare, stencil_read_mask, stencil_write_mask;
            uint8_t stencil_front[4], stencil_back[4];   /* compare, fail, depth_fail, pass */
            int32_t depth_bias; float depth_bias_slope, depth_bias_clamp; } ds;
};
```

- The key is built when a draw is collected (bound shaders + dynamic state +
  the rendering's formats); a new key starts `createRenderPipelineAsync`
  right then, and the pass waits for it only when it encodes the draw, so a
  pass's new pipelines compile in parallel. Kept by key in memory. Not done
  yet: starting creation at `vkCreateGraphicsPipelines` for pipelines with
  no relevant dynamic state, and the persistent cache (IndexedDB) in the page.
- Vertex shader variants: the fragment shader's `fs_varyings` (compiled
  first in a linked pipeline; otherwise the bound one at draw time) and
  `keep_clip_y` (set when the viewport height is negative: wgvk then gives
  WebGPU the positive-height viewport over the same pixels).
- Bind group layouts: T1's `wgvk_bgl_entries_from_shaders` + `wgvk_bgl_get`
  over the pipeline's stages; bind groups: T1's `wgvk_bind_group_get`, with
  T2 listing the buffer ranges and textures behind each entry, and T1's
  `wgvk_bind_group_entries` (added by T2) to rebuild a group with snapshot
  buffers.
- Draw parameters (6.1): written into the immediates at the vertex shader's
  `legal.draw_params_offset` per draw (BaseVertex, BaseInstance, DrawIndex),
  or into the driver uniforms (901). Indirect draws whose arguments the CPU
  has (host-visible memory not written by the GPU in flight) become direct
  draws, which also gives draw counts; GPU-written arguments use WebGPU's
  indirect draws (draw parameters zero, a GPU-written count draws the
  maximum: logged).
- Driver bindings (6.1), from a per-submission driver ring written with one
  `queue.writeBuffer` per 256 KiB chunk: 900 push constants over the
  immediates, 901 `wgvk_driver_uniforms`, 920+b pulled vertex buffers
  (bound from the storage-aligned offset below the binding,
  `vertex_buffer_offset[b]` the rest), 936 the index buffer of expanded
  indexed points. Not yet: 902/910+ (buffer heap: `wgvk_heap_build` is
  there), 940 (poly).
- Vertex shader variants: `struct wgvk_vs_key` (fs_varyings, keep_clip_y,
  depth clip control's [-1, 1] through `nir_lower_clip_halfz`, vertex
  pulling mask + `vi` for the bindings `wgvk_vertex_binding_needs_pulling`
  names or whose bound offset is unaligned, point expansion for point lists
  whose vertex shader writes gl_PointSize).
- Textures WebGPU cannot filter (depth, formats without the filter cap)
  bound where a shader samples float: those layout entries become
  UnfilterableFloat and the samplers of the same combined image samplers
  NonFiltering (a pipeline variant; `wgvk_pipeline_key.unfilterable`).
- Index rewriting (`wgvk_index.c`): triangle fans, provoking vertex "last"
  when the fragment shader has flat inputs (rotation keeps the winding),
  8-bit indices, primitive restart in strips/fans: a list index buffer made
  on the CPU when the indices are there, else by a compute pre-pass.
- **Compute pre-passes are hoisted** (6.2, adopted): a draw may carry
  compute dispatches (`struct wgvk_prepass`: index rewriting now, T4's GS
  parts next); they are encoded in one compute pass just before the WebGPU
  render pass the draw lands in (per split segment, so a split pass gets
  its own pre-passes). No blocker found: within a Vulkan render pass a
  vertex-stage read cannot depend on a write by an earlier draw of the same
  pass, so moving the pre-passes ahead is safe.
- **Geometry shaders (6.2), done:** the vertex shader before a GS is a
  variant with `vs_before_gs` (every attribute pulled, `vi` from the draw);
  the GS is a `vk_shader` holding its NIR, with libpoly programs per
  `wgvk_gs_key` (fs_varyings, keep_clip_y, [-1, 1] depth). Per direct draw
  T2 writes `poly_vertex_params` / `poly_geometry_params` on the CPU into a
  per-draw poly buffer (binding 940; offsets 0 / 128, sink 512, counts,
  vertex outputs and the output index buffer after 1024), dispatches the VS,
  count, pre and main parts as hoisted pre-passes, and draws the rast part
  with `drawIndirect` / `drawIndexedIndirect` from libpoly's draw command.
  Compiled parts get compute pipelines by WGSL + layout (`wgvk_cpipe_get`).
- **Buffer heap (4.4, 6.1), done:** 902 (the table, in the driver ring) and
  910+a (T1's arena windows), dynamic offsets applied; the arenas are
  read-write uses, so direct bindings in the same memory object get
  snapshots. One heap stage per draw: if both stages of a draw use the
  heap, the fragment stage's is bound (they share binding 902).
- **Queries, done:** occlusion queries counted in per-render-pass WebGPU
  query sets (a slot per query and WebGPU pass, so a query survives pass
  splits) and added into per-submission accumulators by a compute shader;
  timestamps by empty compute passes with `timestampWrites` (inside a
  render pass: written after it); `vkCmdCopyQueryPoolResults` by a compute
  shader from the accumulators or earlier submissions' CPU values; results
  given to T1's pools (`wgvk_query_pool_complete`) at completion, unless the
  query was reset since. Pipeline statistics and XFB queries end available
  with zeros.
- **Feedback loops, done:** a draw that samples a subresource its pass
  renders to gets a WebGPU pass of its own and samples a copy made just
  before it (as if a texture barrier came before each such draw); depth
  feedback loops are not copied (logged).
- **Depth copies T1 flags (4.5), done:** buffer -> depth by drawing
  `frag_depth` (D16, D24, D32), depth24plus -> buffer by a compute shader.
- **What Blender's EEVEE needed (M2), done:** passes without attachments
  (`GL_ARB_framebuffer_no_attachments`, EEVEE's shadow passes) get a
  write-masked R8Unorm stand-in target sized from the render area (in the
  pipeline key; a pipeline with colour targets and no fragment shader gets an
  empty one); NaN and infinite clear colours, which WebGPU refuses, clear to
  0 and a driver quad writes the exact bits; a dispatch that samples a
  texture range it also writes as storage (mip chains built level by level)
  reads a copy made before its compute pass; colour write masks and blending
  ignore `cb.attachment_count` (zink's EDS3 blend state leaves it 0).
- **Image buffers (T3b's IMAGE_BUFFER), done (no Blender shader hit it
  yet):** a buffer-typed layout entry with a storage image descriptor binds
  the view's image buffer (`wgvk_image_view_ibuf`: header {w, h, layers, 0},
  texels of the view's level). The image remembers which view's buffer holds
  its latest texels (`ib_owner`) and a change count of its texture
  (`ib_gen`); before a draw's pass or a dispatch, `wgvk_image_ibuf_use`
  copies the texture in when the buffer is stale, and `wgvk_image_before`
  (every other use of the image: sampled, storage, attachment, copies,
  clears, blits, resolves) copies an owner's texels back. Rows that are not
  256-byte multiples are copied one by one (logged past 4096 rows). The same
  hook makes T1's view-format alias textures current
  (`wgvk_image_make_current`, which nothing called before).
- Not done yet: multiview (not advertised; T3b's side is in, 6.4), indirect
  draws with a GS (libpoly's indirect setup kernel, see 6.2: EEVEE's shadow
  pass needs it; T4's side is ready, with GPU draw parameters), indirect draws needing index rewriting, depth/stencil
  resolves, image-to-image copies of depth24plus, primitive restart in
  GPU-side rewrites, snapshots of driver bindings, the variant keys
  `texel_buffer_format` / `storage_image_format` (T2 does not set them yet:
  texel buffers decode as RGBA32; Blender's default scenes do not use
  them).

**Render passes and usage scopes (design risk 1).** A pass's draws are
collected and encoded when it ends. A draw that reads a GPUBuffer an earlier
draw of the pass wrote (or writes one it read) starts a new WebGPU pass that
loads what the last stored ("split"). A draw that reads and writes the same
GPUBuffer (one VkDeviceMemory is one GPUBuffer, so suballocated buffers meet
here) reads copies of its read ranges, made before its pass ("snapshot");
dispatches likewise. A draw that samples what its pass renders to is counted
(`feedback_loops`) but not fixed yet. `WGVK_STATS=1` prints the counts at
vkDestroyDevice; `wgvk_device_stats()` returns them.

**Host-visible memory.** Before the submission, the ranges its commands read
are written into the GPUBuffer (`wgpuQueueWriteBuffer` from a HOST window,
`rvgpu_guest_upload_buffer` from a GUEST one), once per range and
submission. Ranges the GPU writes are copied to MAP_READ buffers at the end
of the same command buffer, mapped when the work is done and copied into the
window (`rvgpu_guest_from_mapped` for GUEST) before the submission's syncs
move; until then uploads leave those ranges alone.

**Completion (T2's, with T1's sync design; 4.6).** No wait in
`vkQueueSubmit`: each gets a serial (`wgvk_queue.submitted`) and completes
in order when its futures fire; syncs keep signals pending on serials,
`wgvk_sync.on_value` runs when a value moves. For front ends (wgvk.h does not
declare them yet: declare them where you call them):

```c
/* fn(data, serial) after submission `serial` (1, 2, ...) completed: downloads written, syncs moved */
void wgvk_device_set_completion(VkDevice device, void (*fn)(void *data, uint64_t serial), void *data);
uint64_t wgvk_device_last_serial(VkDevice device);   /* the serial of the last vkQueueSubmit */
unsigned wgvk_device_poll(VkDevice device);          /* non-blocking: retire what WebGPU finished; returns
                                                        the submissions still in flight (the page's loop) */
const struct wgvk_stats *wgvk_device_stats(VkDevice device);
/* module-wide forms (wgvk.h), what the page uses: rvgpu-worker.js loads the IndexedDB records before the
 * front ends make their devices, and calls wgvk_poll_all (rvgpu_poll) after each ring drain */
void wgvk_set_pipeline_cache(void (*store)(void *data, const void *record, size_t size), void *data,
                             const void *blob, size_t size);
unsigned wgvk_poll_all(void);
/* per device: the persistent pipeline cache: store(data, record, size) for each new render pipeline (the page
 * keeps the records, IndexedDB); preload hands them back at the next start (records back to back) */
void wgvk_device_set_pipeline_cache(VkDevice device, void (*store)(void *data, const void *record, size_t size),
                                    void *data);
unsigned wgvk_device_preload_pipelines(VkDevice device, const void *blob, size_t size);
```

Natively `WGVK_PIPELINE_CACHE=FILE` does the same with a file; `WGVK_STATS=1`
prints the counters at exit; `WGVK_TRACE=1` prints every replayed command,
pass and draw; `WGVK_DUMP_WGSL=1` every module.

T2 added to T1's pieces (4.3): `wgvk_bind_group_entries()` (the entries of a
bind group without making it, for snapshot rebuilds); the pipeline cache
UUID from the build (Mesa's disk cache keeps wgvk's shader objects under
zink; an older build's crashed it). For T1: `largePoints` and
`shaderDrawParameters` can be advertised now (T3b expands points and lowers
draw parameters, T2 drives both).

- The draw hook for T4 (proposed):
  `bool wgvk_draw_hook(struct wgvk_cmd_buffer *, const struct wgvk_draw *)`.
  It returns true when poly took the draw.

## 6. Shaders (T3) - stub (T3a: implemented; T3b's passes plug in before it)

Code: `gpu/webgpu/mesa/src/webgpu/compiler/` (`libwgvk_compiler`, `idep_wgvk_compiler`).
`wgvk_compile.h` is the interface; `nir_to_wgsl.h` documents the NIR shape the
emitter accepts (what T3b's legalisation must hand it). What differs from
version 1 of this section is marked *(T3 proposal)*.

```c
/* spirv_to_nir options and the NIR compiler options wgvk gives Mesa's runtime */
const nir_shader_compiler_options *wgvk_nir_options(void);
const struct spirv_to_nir_options *wgvk_spirv_options(void);   /* UBO/SSBO: vec2_index_32bit_offset */

struct wgvk_compile_caps {             /* (T3 proposal) the device's WebGPU features that change WGSL */
   bool shader_f16, subgroups, clip_distances, dual_source_blending, primitive_index;
   uint32_t max_immediate_size;        /* 64 */
};
/* T1 implements this from the device (wgvk_instance.c: the WebGPU device's features and
 * maxImmediateSize; WGVK_DEBUG=caps prints them); the compiler's weak default returns none */
const struct wgvk_compile_caps *wgvk_device_compile_caps(struct wgvk_device *dev);

struct wgvk_shader_key {               /* what a variant depends on; zero = default */
   const struct wgvk_layout_remap *layout;   /* NULL: group = set, binding = binding, element 0 only */
   uint8_t storage_image_format[16];   /* (T3b) */
   uint8_t provoking_last;             /* (T3b) */
   uint8_t vertex_pulling_mask;        /* (T3b) */
   uint8_t draw_params;                /* (T3b) */
   uint8_t point_size;                 /* (T3b/T4) */
   uint8_t fs_varyings[32];            /* (T3 proposal) vertex shaders: what the fragment shader reads per
                                          location: WGVK_VARYING_USED | interp | sampling << 2 | type << 4 */
   uint8_t keep_clip_y;                /* (T3 proposal) vertex shaders: do not negate gl_Position.y */
};
struct wgvk_shader_binding {           /* one per WebGPU binding the WGSL declares */
   uint8_t group; uint16_t binding;
   uint8_t kind;                       /* enum wgsl_binding_kind: uniform, storage, read-only storage,
                                          texture, storage texture, sampler, comparison sampler */
   uint8_t view_dimension;             /* enum glsl_sampler_dim (1D images are 2D in wgvk) */
   uint8_t arrayed, multisampled;      /* (T3 proposal) */
   uint8_t sample_type;                /* float, sint, uint, depth */
   uint16_t format;                    /* (T3 proposal: uint16) storage textures: enum pipe_format */
   uint8_t access;                     /* ACCESS_NON_READABLE / ACCESS_NON_WRITEABLE */
   uint32_t min_binding_size;
   uint8_t vk_set; uint16_t vk_binding, vk_index;   /* (T3 proposal) where it came from */
   uint16_t vk_array_size;             /* (T3 proposal) elements the shader declares at that binding */
   bool sampler_of_combined;
};
struct wgvk_compiled_shader {
   char *wgsl; size_t wgsl_len;        /* entry point "main" */
   char *error;                        /* (T3 proposal) on failure: one "[tag] detail" line per construct */
   struct wgvk_shader_binding *bindings; unsigned n_bindings;
   uint32_t variant_mask;              /* WGVK_VARIANT_*: which key fields this shader read */
   uint32_t immediate_bytes;
   uint16_t workgroup_size[3];
   uint64_t hash;                      /* XXH64 of the WGSL */
   uint8_t fs_varyings[32];            /* (T3 proposal) fragment shaders: for the vertex shader's key */
};
/* Takes ownership of nir. VK_ERROR_FEATURE_NOT_PRESENT + out->error when something is not supported yet.
 * Natively WGVK_TINT=1 checks the WGSL with Tint (WGVK_TINT_PATH). */
VkResult wgvk_compile(struct wgvk_device *dev, nir_shader *nir, const struct wgvk_shader_key *key,
                      struct wgvk_compiled_shader *out);
VkResult wgvk_compile_with_caps(const struct wgvk_compile_caps *caps, nir_shader *nir,
                                const struct wgvk_shader_key *key, struct wgvk_compiled_shader *out);
void wgvk_compiled_shader_free(struct wgvk_compiled_shader *s);
/* the fragment shader's inputs from its variables, before compiling (same encoding as fs_varyings) */
void wgvk_gather_fs_varyings(nir_shader *fs, uint8_t out[32]);
```

- Graphics pipelines compile the fragment shader first and pass its
  `fs_varyings` into the vertex shader's key: WebGPU requires both sides of
  every location to agree on type and interpolation, Vulkan only reads the
  fragment side. A location the fragment shader reads and the vertex shader
  does not write is declared (zero) in the vertex shader.
- Clip space: Vulkan's y points down, WebGPU's up, so vertex shaders negate
  `gl_Position.y` before returning; the pipeline keeps Vulkan's `frontFace`
  (measured with draw-run: `gl_FrontFacing` and every pixel agree with
  lavapipe). A pipeline that flips through the viewport instead (Vulkan
  allows negative viewport heights, WebGPU does not) sets `keep_clip_y`. (T2)
- The binding remap: element `i` of (set, binding) is `wg_binding + i`; the
  sampler half of a combined image sampler is `wg_sampler_binding + i`.
- Buffers are word arrays in WGSL (uniform `array<vec4<u32>, N>`, storage
  `array<u32>` or `array<atomic<u32|i32>>`); `min_binding_size` is what the
  shader declares (uniform: the block's size; storage: 4).
- Push constants are `var<immediate>` (a struct of vec4<u32>), constant
  offsets only; `immediate_bytes` is their size.
- Compute shaders: an `if` around a workgroup barrier whose condition WGSL
  cannot prove uniform gets its condition through `workgroupUniformLoad` (one
  extra workgroup word and two barriers); Vulkan already requires it uniform.

T4 adds stages (GS, TCS/TES, VS-as-compute) through the same call. The
`nir_shader_compiler_options` wgvk gives Mesa's runtime are T3's.

### 6.1 What the legalisation passes need from the driver (T3b) - proposed

`wgvk_compile.h` has the full declarations; `wgvk_compiled_shader.legal`
(`struct wgvk_legalize_info`) says what a compiled shader needs, and every
`wgvk_shader_binding` carries `lowered` (`enum wgvk_lowered_kind`) when a
pass changed what the binding is.

- **Driver set.** Driver bindings appear in the shader as descriptor set
  0xff (`WGVK_DRIVER_SET`); the remap puts them at `(driver_group, binding)`:
  900 push constants past the immediates (uniform, same offsets), 901
  `struct wgvk_driver_uniforms` (uniform), 902 the heap table (uniform,
  uvec4 per slot), 910+a heap arenas (storage), 920+b pulled vertex
  buffers, 936 the index buffer of an expanded point draw. The compiled
  binding list has them with `vk_set` 255.
- **Immediates.** The app's push constants at [0, pc), then
  `struct wgvk_draw_params` {base_vertex, base_instance, draw_id} at
  `legal.draw_params_offset` (= align4(pc) when 12 more bytes fit, else
  `draw_params_in_ubo`: offset 0 of 901). Push constants larger than the
  immediates (`push_constants_in_ubo`) all go to 900. The rule depends only
  on `layout->push_constant_bytes`, so all stages of a pipeline agree. T2
  writes the draw parameters per draw (indirect draws: a compute pre-pass).
- **Variant keys** (T2 recompiles when they change; `variant_mask` says
  which a shader read): `storage_image_format[i]` (`enum pipe_format`, now
  16-bit) for the formatless storage image at `legal.format_slot[i]`;
  `texel_buffer_format[i]` (new) for the texel buffer at
  `legal.texel_slot[i]`. Zero gives RGBA32 of the image's type.
- **Lowered bindings** (T1 makes the bind group entry, T2 keeps contents):
  - `IMAGE_BUFFER`: a storage image used atomically is a read-write storage
    buffer: {u32 width, height, depth-or-layers, 0} then 32-bit texels of the
    view's level, x fastest, no padding. The driver copies image ↔ buffer
    around the commands that use it (`copyTextureToBuffer` with
    bytesPerRow = 4·width is not 256-aligned in general: a small compute or
    row-by-row copies).
  - `TEXEL_BUFFER`: uniform texel buffer → read-only storage buffer, storage
    texel buffer → storage buffer, over the view's range (size must be a
    multiple of 4; the shader takes the texel count from it).
  - `UBO_AS_STORAGE`: a uniform buffer past `caps.max_uniform_buffers`, bound
    as read-only storage (give uniform buffers STORAGE usage too).
  - `HEAP`: a storage buffer past `caps.max_storage_buffers`, read through
    the heap: entry `heap_slot` of the table at 902 is {arena, byte offset,
    byte size, 0}, the bytes are in arena binding 910+arena
    (`legal.heap_arenas` of them). T1: allocate storage-capable memory from
    a few large arena GPUBuffers so any SSBO descriptor can be expressed.
- **Binding limits (proposal to T1).** Build each pipeline's bind group
  layouts from the compiled shaders' binding lists (only what a stage uses,
  visibility = the stages that use it), not from the whole descriptor set
  layout; bind groups are then made per (set contents, pipeline layout),
  cached. With that, no Blender module needs the heap: the most any module
  uses is 8 storage buffers (limit 10) and 5 uniform buffers (12). The heap
  is the fallback for a stage that really uses more.
- **Vertex pulling.** T2 sets `key.vi` (the pipeline's vertex input: per
  location binding/format/offset, per binding stride/rate/divisor) and
  `key.vertex_pulling_mask` with the bindings
  `wgvk_vertex_binding_needs_pulling()` names (divisor other than 1, a format
  WebGPU's vertex state lacks, stride not a multiple of 4 or over 2048, an
  unaligned offset). Those bindings are bound as read-only storage at driver
  binding 920 + b, starting `vertex_buffer_offset[b]` bytes in (driver
  uniforms); `legal.pulled_locations` are left out of the WebGPU vertex
  state. Instance-rate pulls read the base instance from the draw parameters.
- **Points.** A fragment shader that reads `gl_PointCoord` reads it from
  varying `legal.point_coord_location` (noperspective vec2). For a point
  list with large points T2 sets `key.point_size` (`WGVK_POINTS_EXPAND`,
  or `..._INDEXED16/32`), `key.vi` and `key.point_coord_varying` (the
  fragment shader's location + 1), then draws `draw(6 * count, instances,
  6 * first, firstInstance)` as a triangle list with culling off (indexed:
  first = firstIndex, the index buffer as read-only storage at 936 from
  `index_buffer_offset`, the vertex offset as the draw's base vertex). Every
  attribute is pulled. The driver uniforms carry `viewport_inv_size`
  (1/width, 1/height) and `point_size_range`.
- **Provoking vertex "last"** needs no shader change: T2 rotates each
  triangle's indices (a, b, c) → (c, a, b), which keeps the winding.
- `caps` gains `max_storage_buffers`, `max_uniform_buffers`, `heap_arenas`.
- **T1's answer (agreed, with one amendment):** the bind group layouts come
  from the compiled shaders (`wgvk_bgl_entries_from_shaders`, 4.3); the
  caps are filled from the device (`heap_arenas` = `WGVK_HEAP_ARENAS` = 2);
  the heap table and arenas are `wgvk_heap_build` (4.4), where an arena is
  a memory object's GPUBuffer window rather than a shared large buffer
  (usage scopes). The driver set 0xff → (`driver_group`, 900..999): the
  remap's `driver_group` is the group after the app's sets while one is
  free, else group 3 shared with set 3 (4.2; T3b's `wgvk_layout_remap_from_nir`
  uses the highest set, which only tools read). The driver UBO layout
  (`struct wgvk_driver_uniforms`) stays T3b's.

**Early fragment tests (T2's proposal, done).** WGSL has no
`early_fragment_tests`, so a fragment shader that writes storage does so
for fragments the depth test then rejects (EEVEE's Position and
Environment passes: `imageStore` under `layout(early_fragment_tests)`).
With `key.early_depth` (1; 2 for a multisampled depth attachment, which
tests sample 0) a fragment shader that has `early_fragment_tests` and
writes memory starts with the test itself and reports
`legal.early_depth_test`: the depth at its pixel from driver binding
**938** (`texture_depth_2d` / `_multisampled_2d`; 941 is the first XFB
buffer), compared with the fragment's depth by driver uniform
`depth_compare` (the `VkCompareOp`; ALWAYS when the test is off), as the
format stores it (`depth_unorm_max` = 65535 / 16777215 for unorm formats,
0 for depth32float); `discard` when it fails. T2 sets the key when the
pipeline tests depth, binds the pass's depth attachment there with
`depthReadOnly` on the pass (legal when the pass writes no depth; when it
does, split before the draw and bind a copy, as T2 proposed) and writes
the two words. The pipeline's own depth test still decides the colour.
Scenes `earlyd16` (a d16 pattern, LESS_OR_EQUAL) and `earlyd32` (d32,
GREATER), an image-atomic count per pixel: equal to lavapipe (without the
key 1216 and 1249 texels differ). Stencil is not part of the shader test.

### 6.2 Geometry shaders through libpoly (T4 with T2) - proposed

`wgvk_compile_gs(caps, gs_nir, key, &prog)` returns `struct wgvk_gs_program`:
four parts (`WGVK_GS_RAST` vertex shader, `_MAIN`, `_COUNT`, `_PRE` compute
shaders; `wgsl == NULL` when a part is not needed) and libpoly's
`poly_gs_info` fields (`shape`, `mode`, `max_indices`, `topology`,
`count_words`). The vertex shader before it is compiled with
`key.vs_before_gs` (and `key.vi`, every binding in `vertex_pulling_mask`):
it becomes a compute shader, 64 wide, that writes its outputs
(`legal.vs_outputs`) to libpoly's vertex output buffer.

Memory: one **poly buffer** (usage Storage | Index | Indirect | CopyDst),
bound at driver binding 940; libpoly's structs (`src/poly/geometry.h`) live
in it at offsets the driver uniforms give (`poly_vertex_params`,
`poly_geometry_params`, `poly_sink`: 64 zero bytes). An address in those
structs is `segment << 32 | byte offset`: segment 0 = the poly buffer,
1 = the draw's index buffer (bound at 936). For a direct draw T2:

1. writes `poly_vertex_params`: `poly_vertex_params_init(vp, vs_outputs,
   {64,1,1})`, `poly_vertex_params_set_draw(vp, count, instances)`,
   `output_buffer` = room for `poly_tcs_in_size(count * instances,
   vs_outputs)` bytes; indexed: `index_buffer` = `1 << 32 | firstIndex *
   index size`, `index_size_B`, `index_buffer_range_el`;
2. writes `poly_geometry_params`: `poly_geometry_params_init(gp,
   input prim, {64,1,1})`, `poly_geometry_params_set_draw(gp, prim, shape,
   max_indices, count, instances)`, every counter pointer to the sink;
   `POLY_GS_SHAPE_DYNAMIC_INDEXED`: `output_index_buffer` = room for
   `draw.index_count` u32, `draw.first_index` = its offset / 4;
   `_STATIC_INDEXED`: `topology` widened to u32 (0xff = restart) somewhere
   in the buffer, `draw.first_index` there; `count_words`: `count_buffer`
   (and `count_buffer_stride` = 4 · count_words), zeroed;
3. sets the draw parameters as for any draw (base vertex = vertexOffset
   indexed, firstVertex otherwise; base instance), and the poly offsets in
   the driver uniforms;
4. encodes, **before the render pass** the draw is in (all GS pre-passes of a
   render pass can be hoisted ahead of it): compute dispatches of the VS
   part `(ceil(count/64), instances)`, `_COUNT` and `_MAIN`
   `(ceil(prims/64), instances)` with prims = `gp.grid[0]`, `_PRE` `(1,1,1)`;
5. in the render pass: the rast part with the fragment shader, topology
   `prog.mode` (strips with `stripIndexFormat` Uint32 when indexed), the
   poly buffer as index buffer (Uint32) for indexed shapes, and
   `drawIndexedIndirect` / `drawIndirect` at `poly_geometry_params +
   offsetof(draw)` (libpoly's `poly_indirect_draw` is WebGPU's argument
   layout).

**Indirect draws through a GS.** `wgvk_compile_gs_setup(caps, &shader)`
(one 1x1x1 compute shader for every GS: libpoly's
`poly_gs_setup_indirect` plus the dispatch counts, from
`gpu/webgpu/mesa/src/webgpu/poly/wgvk_poly.cl`, built by mesa_clc).
T2 instead of the direct steps 1-4:

- copies the app's 16/20-byte draw command into the **poly indirect
  buffer** (usage Storage | Indirect, driver binding 937, segment 2) at
  `poly_indirect_draw` (`copyBufferToBuffer`, before the compute pass);
  the setup writes 9 words at `poly_indirect_dispatch` there: the VS
  part's, the GS parts' and the XFB part's workgroup counts (they cannot live in the poly buffer: a dispatch may not
  read its indirect arguments from a buffer it binds writable);
- writes `struct wgvk_gs_indirect_args` (`poly/wgvk_poly_args.h`) at
  `poly_setup_args`, a `struct poly_heap` at `poly_heap` with **base 0**
  (libpoly makes the GS index buffer's first index relative to the heap
  base, and the draw binds the whole poly buffer) and `bottom` past the
  fixed structs, `poly_vertex_params_init` / `poly_geometry_params_init`
  as before, `vp.index_size_B` (the setup sets the index buffer, not its
  size), counters to the sink, `count_buffer` when `count_words`; and
  the draw's `draw_id` in the args;
- sets driver uniform `poly_draw_params` to 12 free bytes of the poly
  buffer: the setup writes {base vertex, first instance, draw id} there
  from the command, and the VS part reads its draw parameters there
  (the immediates' draw parameters are ignored; 0 = a direct draw);
- dispatches the setup `(1,1,1)`, then the VS part with
  `dispatchWorkgroupsIndirect(937, poly_indirect_dispatch)`, the count/main
  parts with `+ 12`, the pre part `(1,1,1)`, then the rast draw as for a
  direct draw.

`drawCount > 1` (multi-draw indirect): one setup + draw per command, with
`poly_indirect_draw` at the next command. EEVEE's shadow draws are this
path (scene `blendereeveeind`: Blender's own shadow VS + GS + FS drawn
indirectly, first vertex 3, first instance 1; `gstriind` indexed with base
vertex 2 and first instance 2; the CPU's draw parameters poisoned in all
three: equal to lavapipe).

**Vertex shaders that write memory (6.3).** `wgvk_compile` fails with
`legal.vs_writes_memory`; `wgvk_compile_vs_compute(caps, vs, key, &prog)`
gives `prog.compute` (as with `vs_before_gs`) and `prog.rast`, a
passthrough vertex shader. T2: vertex params as for a GS draw, dispatch the
compute part `(ceil(count/64), instances)`, then draw `prog.rast`
non-indexed `(count, instances, 0, 0)` with the original topology. (Strips
with primitive restart from such a shader: not handled yet.)

**Fragment shaders without a colour output** (EEVEE's shadow pass): WebGPU
needs `writeMask` 0 on such targets (Vulkan ignores the mask); T2 can see it
in the compiled fragment shader's WGSL / outputs.

**Transform feedback.** A vertex shader with feedback outputs reports
`legal.xfb` and its `xfb_info`; without an application GS T2 compiles it
with `key.vs_before_gs` and calls `wgvk_compile_xfb_gs(caps, &vs, prim, key,
&prog)` (libpoly's passthrough GS), then draws as for a GS. `prog` then has
an `_XFB` part (dispatched after `_MAIN`: `(ceil(draw.index_count/64),
draw.instance_count)`, or indirectly at `poly_indirect_dispatch + 24`) and,
when counts are not static, a `_PREFIX` part (1x1x1, after `_COUNT`, before
`_PRE`; the count buffer is then `count_buffer_stride` per input primitive).
T2 sets per buffer: `xfb_base_original[b] = (3 + b) << 32` (+ byte offset),
`xfb_size[b]`, `xfb_offs_ptrs[b]` = a word in the poly buffer holding the
current write offset (VK_EXT_transform_feedback's counter buffer value), and
binds the buffer at driver binding 941 + b. Query results: point
`prims_generated_counter[s]` / `xfb_prims_generated_counter[s]` at words of
the poly buffer (needed / written), and copy them to the query pool.

`tools/scene-run.c` (`wgpu_draw_gs`) does all of this and is the reference.

**Tessellation (6.5).** `wgvk_compile` of a TCS gives a compute shader
(workgroup = `legal.tess.tcs_vertices_out`, dispatched `(patches per
instance, instances)`) and reports `legal.tess` {`tcs_vertices_out`,
`nr_patch_outputs`, `output_stride`, `per_vertex_outputs`}; of a TES, the
vertex shader that draws, with `legal.tess` {`domain`, `spacing`, `ccw`,
`point_mode`}. The VS before them is compiled with `key.vs_before_gs` (as
for a GS). `wgvk_compile_tess_kernels(caps, domain, out[3])` gives the
count kernel, the prefix sum and the tessellate kernel (per domain, cache
them); kernels [0] and [2] report `legal.scratch_bytes` (private memory per
invocation, a few hundred bytes; the kernels zero their own). T2, per draw:

- in the poly buffer: `poly_vertex_params` as for a GS, a `poly_heap` with
  base 0, and a `struct poly_tess_params` (as `hk_upload_tess_params`):
  `heap`, `patch_coord_buffer` = 0, `coord_allocs`, `counts`, `out_draws`
  (20 bytes: the draw command), `tcs_buffer` (`output_stride` per patch),
  the sizes from `legal.tess` and the draw, partitioning from `spacing`
  (equal = INTEGER, fractional odd/even), `points_mode`, `isolines`, `ccw`
  (0 for points and isolines), `patches_per_instance`, `nr_patches`;
- driver uniforms `poly_vertex_params`, `poly_tess_params`, `poly_sink`,
  and `poly_scratch`: a region of `scratch_bytes * ceil(nr_patches / 64) * 64`
  bytes;
- dispatch the VS part `(ceil(count/64), instances)`, the TCS `(patches,
  instances)`, count `(ceil(nr_patches/64), 1)`, prefix `(1,1,1)`,
  tessellate `(ceil(nr_patches/64), 1)`;
- draw the TES with `drawIndexedIndirect(poly, out_draws)`, the poly buffer
  bound as a Uint32 index buffer, topology triangle list (line list for
  isolines, point list for point mode). `tools/scene-run.c`
  (`wgpu_draw_tess`) is the reference.

**Multiview (6.4, T2's proposal, shader side done).** A shader that reads
`gl_ViewIndex` reports `legal.view_index` and reads the view from driver
uniform `view_index` (byte 136 of `struct wgvk_driver_uniforms`, which is
812 bytes with 6.6 and the early depth test). T2 renders the subpass once
per bit of the view mask into that layer of each attachment (a 2D view of layer N), writing `view_index`
= N before each. Nothing else in the shader changes (no layer output is
written). Input attachments read the bound view, so T2 binds the per-view
layer view there too. `tests/scene/multiview` renders view 2 of a
three-layer target on lavapipe (viewMask 0b100) and with `view_index` 2 on
Dawn: equal.

**Multiple viewports and layers (6.6, T2's proposal, done).** A vertex
stage that writes `gl_ViewportIndex` or `gl_Layer` (a VS, a GS through its
rast part) reports `legal.writes_viewport_index`; without a key they are
dropped (viewport 0, layer 0), as before. To emulate them T2 compiles the
fragment shader with `key.viewports` = 1: it reports
`legal.viewport_index_location`, a varying (flat uvec2: viewport, layer)
it reads and discards outside viewport i and scissor i and when the layer
is not driver uniform `layer` (`gl_ViewportIndex` / `gl_Layer` inputs read
it too). The last vertex stage gets `key.viewport_index_varying` = that
location + 1: it maps `gl_Position` from viewport i into the render area
and writes (i, layer) there. T2 writes into the driver uniforms (now 812
bytes) `render_area` (x, y, w, h of the WebGPU viewport it sets: the whole
render area, positive height, depth 0..1, and no `keep_clip_y`),
`viewports[16]` (Vulkan's x, y, w, h (h may be negative), minDepth,
maxDepth), `scissors[16]` (x, y, w, h), and for a layered pass renders one
WebGPU pass per layer (a 2D view of it) with `layer` = N. All vertices of a
primitive must have the same index (they do for a GS; a vertex shader
writing them per instance, as layered/instanced renderers do). Depth
clipping is to 0..1, not to the viewport's range. Scenes `gsviewports` (4
viewports: offsets, a negative height, depth ranges incl. reversed,
scissors), `gslayers` (layer 1 of 2, a GS writing both), `vsviewport` (a
vertex shader writing both per instance): equal to lavapipe.

**gl_PrimitiveID written by a GS (6.7, for T7's glxgears through zink).**
The GS's rast part reports `legal.writes_primitive_id`; without a key the
value is dropped (zink's GS passes it on; glxgears' fragment shader does not
read it). When the fragment shader of a pipeline with a GS reads
`gl_PrimitiveID`, T2 compiles it with `key.primitive_id_from_varying` = 1
(it reads a flat u32 at `legal.primitive_id_location` instead of
`@builtin(primitive_index)`) and the GS with `key.primitive_id_varying` =
that location + 1. Without a GS the fragment shader's `gl_PrimitiveID` stays
`@builtin(primitive_index)` (the WebGPU feature `primitive-index`). Scenes
`gsprimid` (the GS writes gl_PrimitiveIDIn * 3 + 1, the fragment shader
colours by it) and `gsprimiddrop` (glxgears' case): equal to lavapipe.

**For T4, from Blender's EEVEE on wgvk (T2, 2026-10-10): both done.**
EEVEE's shadow pass draws once per sample with `vkCmdDrawIndexedIndirect`
(a culling pass writes the arguments) through Blender's own GS writing
`gl_ViewportIndex`. (1) Indirect GS draws: the setup kernel and its
outputs are above ("Indirect draws through a GS"); the command is copied
into the poly indirect buffer rather than read in place, and the draw
parameters now come from the GPU too. (2) `gl_ViewportIndex`: 6.6.

## 7. The ring: ops v3 for venus, and caps (T5, T6) - v3 implemented (T5), APPROVED

Ring v2 (gpu path's `gpu/PROTOCOL.md`) is unchanged for virgl. Changes are
proposed here and agreed with the orchestrator and the gpu path's owner
before gpuserver changes. **Orchestrator's approval note:** _(pending: T5
proposes the final v3 below, implemented on `gpu-t5` (page side) and
`gpu-t5-server` (gpuserver, branched from `gpu`); approve here)_.

### 7.1 Ring ops v3 (T5) - final proposal, implemented

**Approved by the orchestrator, 2026-10-10:** header version stays 2; venus detected by vcaps_len > 0; ops 9 VN_CTX_INIT, 10 VN_SUBMIT, 11 VN_BLOB_CREATE; syncs as u64 words answered by gpuserver (SYNC_WAIT returns an eventfd); contexts created on their first record and routed per front end in rvgpu.c. The `gpu` path's owner role for gpuserver passes to the integration owner (T6) now that `gpu` is merged into main.

v3 is v2 plus three ops. Header word 4 (`version`) stays 2: nothing in v2
changes, and an executor says it serves venus by writing the venus capset
(`vcaps_len` > 0, 7.2). Without that, gpuserver answers `GET_CAPSET` with
"no venus" and a venus client stops at its start, so it never sends v3
records to an executor that does not know them (the vrend Worker).

| op | name | payload (u32) | completion |
| --- | --- | --- | --- |
| 9 | VN_CTX_INIT | capset_id (4 = venus), then the client's name (CREATE_RENDERER's, NUL-terminated) | no |
| 10 | VN_SUBMIT | SUBMIT_CMD2 rewritten: n_batches; per batch {cmd_off, cmd_size, sync_off, sync_count, ring_idx, flags} (dword offsets/sizes in this payload; ring_idx 0 unless the client set FLAG_RING_IDX); the command dwords; per sync {addr_lo, addr_hi, value_lo, value_hi} (the address of the sync's u64 word) | yes: when its commands have run |
| 11 | VN_BLOB_CREATE | res, blob_type, blob_flags, size_lo, size_hi, blob_id_lo, blob_id_hi, addr_lo, addr_hi | no |

- **Contexts.** gpuserver makes a client's context with its first record:
  `CTX_CREATE` (op 1, v2) for a virgl client, `VN_CTX_INIT` for a client
  whose CONTEXT_INIT comes first (venus). rvgpu.c routes a context's records
  to the front end that owns it: every context is virgl's until a
  `VN_CTX_INIT` makes it venus' until its `CTX_DESTROY`
  (`rvgpu_ring_register_front(RVGPU_FRONT_VENUS, op, ...)`; venus registers
  ops 2, 4, 9, 10, 11). `RES_UNREF` and `CTX_DESTROY` are v2's.
- **Blobs** (`VN_BLOB_CREATE`). gpuserver makes every blob (a file the guest
  maps, as for virgl backings), puts the record in the ring, and replies
  with the fd only once the executor has taken the record (the ring's tail
  is past it), so the import is done before the guest can touch the pages
  (VK-25: the import's copy below used to land after venus had written its
  fence feedback slot, turning VK_NOT_READY back into 0 = VK_SUCCESS). blob_id
  0: shared memory (venus rings, reply and command streams; vkr reads and
  writes it through `rvgpu_guest_*`). Any other blob_id is the
  VkDeviceMemory with that object id: its bytes are the guest's pages from
  then on (`wgvk_memory_set_guest_window`, 4.1: GUEST window; what the
  memory's own window held is copied there first). Venus creates these
  blobs when the guest maps the memory (vkMapMemory), after a ring wait
  that makes sure the allocation was processed. Blob ids are 64-bit
  (venus object ids), hence a new op rather than v2's `BLOB_CREATE` (8).
- **Syncs need no ops.** A venus timeline sync is a u64 word in gpuserver's
  memory (its own table, after the export table). gpuserver answers
  SYNC_CREATE, SYNC_READ, SYNC_WRITE (and RESET, which is a write) and
  SYNC_UNREF from the words, and SYNC_WAIT with an eventfd it writes once
  the words reach their values (checked every turn; a turn is 1 ms while a
  wait is open). The page writes a word (low half, then high; readers read
  high, low, high) when the batch that signals it is done: vkr's fence on
  the batch's ring_idx (a queue timeline; 0 = the CPU's, at once) retired.
  A word is used again only when no submitted batch will still write it.
  (Mesa 25.0.7's venus over vtest uses renderer syncs only for external
  fences/semaphores, which vtest does not offer, so in practice they are
  idle.) The proposal's ops 11-13 (SYNC_CREATE/WRITE/UNREF) are dropped.
- **No threads in vkr** (INTERFACES 2): what vkr's ring, ring-monitor and
  queue-sync threads did runs from rvgpu's poll hook at the end of every
  drain (`rvgpu_ring_set_poll`): run each venus ring from its head to the
  guest's tail, set ALIVE on monitored rings, poll the driver's completion
  (`wgvk_device_poll`) and retire vkr's fences. While a ring is not idle or
  work is pending, `rvgpu_poll_ms()` is 1 and the Worker drains again after
  1 ms instead of waiting for the doorbell (the guest writes ring commands
  without notifying anyone; it notifies through `vkNotifyRingMESA` only
  once the ring has said IDLE, after its idle timeout).
- **Batch headers.** Mesa 25.0's `struct vcmd_submit_cmd2_batch` is 6 dwords,
  virglrenderer's newer header 8; gpuserver tells them apart by where the
  first batch's commands start.

### 7.2 Caps (T6 for virgl, T5 for venus)

- virgl: `struct virgl_caps_v2` at `caps`, as today, filled by vfront from
  zink's pipe caps (the inverse of `virgl_screen.c`).
  `host_feature_check_version` ≥ 23 (PROTOCOL.md).
- GL 4.3 (VK-03, implemented): the guest reaches GL 4.3 core only with tessellation and 16 viewports in the caps, which
  wgvk's Vulkan does not have; vfront advertises them (`vf_caps.c` `gl43_fakes`, "fake" in the design table's sense;
  `VFRONT_CAPS=honest` gives GL 3.3). Tessellation shaders are dropped and patch draws skipped; only viewport and
  scissor 0 reach the device. STATUS.md T6 lists what passes.
- venus (implemented): the venus capset (`struct virgl_renderer_capset_venus`,
  160 bytes, `vkr_get_capset`) at `caps + 2048`, length in header word 88
  (`vcaps_len`, exec), written by `rvgpu_set_venus_caps`, which also sets
  `exec_state` 1 if no front end has (a page with vkr and no vfront serves
  venus). The caps area is 4096 bytes (gpuserver's `CAPS_SIZE`).
  `GET_PARAM` MAX_TIMELINE_COUNT is 64 (vkr's `sync_queues`).

### 7.3' vkr in the page (T5) - implemented on `gpu-t5`

- Sources: virglrenderer main c8e3fc3 (2026-10-08) + venus-protocol v1.1.3
  (`gpu/webgpu/vkr/virglrenderer.pin`), patched by
  `gpu/webgpu/vkr/virglrenderer-page.diff`; `build/vkr-tree.sh` (called by
  native.sh and wasm.sh) makes `$W/vkr-trees/NAME`, generates the protocol
  headers and links it into the Mesa tree as `src/vkr-src`;
  `mesa/src/webgpu/vkr/meson.build` builds `libvkr` (and natively
  `vkr-vtest`), and rvgpu.wasm links it when present.
- What the patch changes in vkr: shm resources are guest addresses
  (`vkr_guest.h`): rings, the reply stream (written into a copy, flushed into
  the guest after every command and before the ring head moves) and command
  streams (copied out before decoding); no threads (above); Vulkan from a
  given `vkGetInstanceProcAddr` (`wgvk_GetInstanceProcAddr`, no loader);
  host-visible memory is plain memory plus the guest-window hook (no forced
  dma-buf/opaque-fd export, no gbm/udmabuf).
- Entry points: `rvgpu_vkr_start(gipa, set_guest, device_poll)`
  (`vkr/rvgpu_vkr.h`); in the page `rvgpu_vkr_page_start()` (exported),
  which rvgpu-worker.js calls after `rvgpu_ring_attach`. The Worker waits for
  the doorbell `rvgpu_poll_ms()` ms (exported).
- Uses from T1/T2: `wgvk_memory_set_guest_window(VkDevice, VkDeviceMemory,
  guest, size)` (new, in `include/wgvk.h`, written by T5 in wgvk_memory.c),
  `wgvk_device_poll(VkDevice)` (T2; weak: a build without T2 never retires
  work for venus).
- Guest (orchestrator, Debian root): `mesa-vulkan-drivers` (libvulkan_virtio.so,
  `/usr/share/vulkan/icd.d/virtio_icd.riscv64.json`) and `vulkan-tools`;
  libvulkan1 and every library they need are in the daily root already.
  Environment: `VN_DEBUG=vtest VTEST_SOCKET_NAME=$XDG_RUNTIME_DIR/rvgpu-0`
  (and `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.riscv64.json` to
  keep lavapipe out when both are installed). `desktop`'s `RVGPU=1` should
  export the first two when gpuserver's ready file is there.

### 7.3 vfront in the page (T6) - agreed, in `webgpu`

- Build: vfront needs zink's Gallium, so it cannot be built from
  `src/webgpu/meson.build` (processed before `src/gallium`). The
  `patches/mesa-page.diff` hunk in `src/meson.build` adds, after
  `subdir('gallium')`: `subdir('webgpu/vfront')` (when zink is built) and
  then `subdir('webgpu/rvgpu')` (Emscripten), which moves out of
  `src/webgpu/meson.build`: rvgpu links vfront. `rvgpu/meson.build` adds
  `idep_vfront_rvgpu` to `rvgpu_deps` and `_vfront_rvgpu_start` to
  `rvgpu_exports` when vfront was built.
- zink on wgvk without a loader: `patches/mesa-page.diff` adds
  `void zink_set_vk_get_instance_proc_addr(void *gipa)` (zink_public.h,
  zink_screen.c): a non-NULL gipa replaces `util_dl_open(VK_LIBNAME)`;
  vkGetDeviceProcAddr then comes from the instance. vfront passes
  `wgvk_GetInstanceProcAddr`.
- The Worker calls `vfront_rvgpu_start()` once, after `rvgpu_init()` and
  `rvgpu_ring_attach()`: it starts zink on wgvk with no driver threads
  (vfront sets `GALLIUM_THREAD=0`), registers ring ops 1-7, writes the
  virgl caps with `rvgpu_set_caps` and sets exec_state 1; on failure it sets
  exec_state 3 (error 1) and returns -1. Not a JSPI export: screen creation
  does not wait (to be confirmed on wgvk in the page).
- Completion: a SUBMIT or TRANSFER_PUT completes as soon as it is flushed
  (vfront copies everything it reads from backings while decoding), unless
  its flush left readbacks or query results to write; those, and every
  TRANSFER_GET, complete after `vf_ctx_poll` waits for them (wgvk_wait, so
  under `rvgpu_ring_drain` the drain suspends). So `ctx_done` passes a
  record only when the guest can read what it asked for.
- Guest memory: vfront reads and writes backings through
  `rvgpu_guest_read`/`rvgpu_guest_write` (a copy through the module heap;
  `rvgpu_guest_ptr` is NULL in the page). Zero-copy uploads
  (`rvgpu_guest_upload_*`) would need zink-level hooks: later.
- One thread (section 2): besides `GALLIUM_THREAD=0`, zink's compile
  queues (`cache_get_thread`, 4 threads, always made) would start pthreads
  the page has no Workers for and call Vulkan from them. The
  `patches/mesa-page.diff` hunk in `src/util/u_queue.c` adds
  `MESA_UTIL_QUEUE_INLINE=1`: no util_queue starts a thread, every job runs
  in the thread that adds it. vfront sets it (and `MESA_SHADER_CACHE_DISABLE`:
  the disk cache's writer is not safe inline) for `VF_INIT_SINGLE_THREAD`.
  rvgpu links with `-sPTHREAD_POOL_SIZE_STRICT=2`, so a thread anyone
  starts in the page fails loudly instead of waiting forever.
- No loader in the page: zink asks `vkGetInstanceProcAddr(NULL, ...)` for
  `vkEnumerateInstanceLayerProperties`, which a loader provides and wgvk does
  not; zink then gives up without a message. vfront wraps the given gipa
  and answers it (no layers; Vulkan 1.0 without `vkEnumerateInstanceVersion`).
  T1 may prefer wgvk to answer it itself.
- Status on wgvk (2026-10-10, webgpu 353df49): zink starts on wgvk natively
  and in Chrome (Metal adapter); vfront replays a capture's contexts and
  resources there and stops at the first draw: wgvk has no graphics
  pipelines yet (`vk_get_graphics_pipeline_compile_info` calls a NULL
  entry point) and its command buffers skip BeginRendering and friends
  (T2). Caps vfront derives from zink follow whatever wgvk advertises.
- virgl caps notes (7.2): sample counts the driver lacks are rounded up at
  resource creation (virgl's caps cannot say "1 and 4 only"); 32-bit
  SSCALED/USCALED/SNORM/UNORM vertex formats (none in Vulkan) are fetched as
  integers and converted by a vertex shader variant.

## 8. The page Worker (T0) - stub

`tools/www/rvgpu-worker.js` (exists; the ring loop, the smoke test and
T7's present path, section 9, run) speaks gpu-exec.js's Worker messages, so
gpu-exec.js can start it in place of the vrend Worker (`?exec=webgpu`; the
page-side switch is T7's):

- page → worker: `{type:"start", memory, ring, caps, base}`,
  `{type:"grab", ctx, res, present}`, `{type:"shown", serial}`,
  `{type:"table", ...}`.
- page → worker, for tools/www/rvgpu-smoke.html only:
  `{type:"smoke", canvas, base, color}` (an OffscreenCanvas), answered by
  `{type:"smoke", ok, msg, device, adapter, texture, readback, errors, ms}`.
- worker → page: `{type:"ready", renderer, exec:"webgpu", capsLen, errors}`,
  `{type:"present", ctx, res, frame, serial, bitmap, w, h, grabbed, bgra, blitMs, stats}`
  (the bitmap is transferred), `{type:"log", text}`, `{type:"stats", stats}`.
- It owns the one GPUDevice (requested with the adapter's limits that wgvk
  needs: `maxStorageBuffersPerShaderStage`, `maxBindGroups`,
  `maxColorAttachments` and so on at the adapter's maximum; features: every
  one the adapter has that the design table lists).
- Loop: `Atomics.waitAsync` on the ring's doorbell (500 ms timeout, so
  `exec_heartbeat` moves), then the JSPI export `rvgpu_ring_drain()`, then
  the event loop again. No `Atomics.wait` on this thread.
- `exec_state` 1 and the caps are written by a front end (vfront, T6)
  through `rvgpu_set_caps`/`rvgpu_set_state`; until one is linked in,
  gpuserver keeps waiting for them.

## 9. Presenting: slots and the export table (T7, venus side VK-07) - implemented

**The contract with the window server is unchanged** from `gpu/PROTOCOL.md`
(Presenting, stage B): a window whose buffer is a GPU resource has slot
format 2, `buffer_lo` = res, `buffer_hi` = gpuserver's pid, `gpu_serial`
at slot offset **244** (gpuserver's `submitted` when the buffer was
committed); export table entries `{dev, ino_lo, ino_hi, res, ctx}`. The
page draws a slot's frame once the ring's `done` has passed `gpu_serial`,
and writes `uploaded`/`shown` as for any window: the window server fires
frame callbacks on `shown` and releases buffers on `uploaded`.

Which guest programs make such buffers: venus WSI images (the guest's WSI
hands over the dma-buf of the image's memory: a blob's backing file), and
any client that hands over a virgl resource's backing file
(`gpu/tools/present-test.py dmabuf`). Stock Mesa GL on virpipe presents
through wl_shm (stage A: TRANSFER_GET, then a copy), which vfront serves as
before; GL zero copy needs the emulated render node (PROTOCOL.md,
rv-virtgpu), not this side.

**Producers** (`rvgpu/rvgpu.h`):

```c
struct rvgpu_present_image {
	WGPUTexture tex;		/* not referenced: valid until the producer next changes the resource */
	uint32_t width, height;		/* the picture, from the texture's origin (level 0, layer 0) */
	uint32_t flags;			/* RVGPU_PRESENT_Y_0_TOP: row 0 is the top; else the bottom (flipped) */
};
void rvgpu_present_register(uint32_t res, WGPUTexture tex, uint32_t flags);   /* named outright */
void rvgpu_present_unregister(uint32_t res);
typedef bool (*rvgpu_present_resolve_fn)(void *front, uint32_t res, struct rvgpu_present_image *out);
void rvgpu_present_add_resolver(rvgpu_present_resolve_fn fn, void *front);     /* found when asked */
bool rvgpu_present_find(uint32_t res, struct rvgpu_present_image *out);
/* page only: res is the blob of venus memory `memory` of wgvk device `device` (64-bit handles); its
 * window shows the image bound to that memory, top row first */
void rvgpu_present_register_memory(uint32_t res, uint64_t device, uint64_t memory);
```

- vfront (in `webgpu/vfront/vf_present.c`, added by `vfront_rvgpu_start`)
  resolves a virgl resource: `vf_res_pipe(res)` → zink's VkImage
  (`zink_resource_get_vk_image`, a `patches/mesa-page.diff` hunk in
  zink_public.h / zink_resource.c) → `wgvk_image_present_texture`. Always
  `Y_0_TOP`: zink's image rows are the Gallium resource's, and a window
  buffer's row 0 is its top (Mesa's state tracker draws window framebuffers
  upside down for that; readbacks fill the backing in the same order).
- **vkr (venus), implemented (VK-07).** rvgpu_vkr.c's guest-memory hook calls
  `rvgpu_present_register_memory(res, device, memory)` for the blob VN_BLOB_CREATE is importing, and
  `RES_UNREF` calls `rvgpu_present_unregister(res)`; both in the page and in the native test bed. Nothing
  else: wgvk remembers which image was bound to which memory (the registry only stores the handles; the
  memory may be freed before the blob is unref'd, so `wgvk_memory_image` never dereferences it and answers
  NULL then).
  - **The blob must be accepted for any memory, not only host-visible.** Mesa's WSI (`wsi_select_device_
    memory_type`) denies host-visible memory types to a native window image, so a venus WSI image's memory
    is device-local, and `virglrenderer-page.diff` used to refuse such blobs ("cannot be a blob"): no
    window image ever reached a window. Now `vkr_context_import_guest_resource` takes any
    `VkDeviceMemory` that the blob does not exceed; for device-local memory the guest hook's
    `wgvk_memory_set_guest_window` does nothing (no window), and only the present registry matters.
  - **Page:** `memory_resolve` (rvgpu_page.c) finds the image when a window shows the blob; the Worker draws
    the texture (below).
  - **Native test bed** (vkr-vtest; no page, so no Worker to draw): the window server in the test (weston)
    reads the dma-buf of the memory, which is the blob's memfd, so vkr-vtest implements
    `rvgpu_present_register_memory` itself and, through `wgvk_device_set_retire_hook`, reads every
    registered window image back (`wgvk_image_read_present`: level 0, layer 0, rows at the image's own
    linear pitch = `VkSubresourceLayout.rowPitch`, 256-aligned) into the blob's pages each time a wgvk
    submission retires, **before** that submission's downloads reach the guest (so before the guest can
    learn that the frame is done). About 0.7-1.1 ms per image read-back.
  - Test: `vkr/scripts/venus-wsi.sh` (`tests/wsi.test` runs it): weston's headless backend **with its GL
    renderer** (the pixman renderer has no zwp_linux_dmabuf_v1, so Mesa's WSI finds no format and
    `vkcube` cannot make a swapchain), vkr-vtest, and the trixie arm64 chroot's `vkcube --wsi wayland`
    through venus: 100 frames must finish, and `weston-screenshooter` must show the textured cube.

**wgvk** (`include/wgvk.h`, `vulkan/wgvk_present.c`):

```c
WGPUTexture wgvk_image_present_texture(VkImage image, uint32_t *width, uint32_t *height);
VkImage wgvk_memory_image(VkDevice device, VkDeviceMemory memory);   /* the image bound last, or NULL */
/* native test beds only (no page to draw the texture): */
VkResult wgvk_image_read_present(VkImage image, void *dst, uint64_t dst_size, uint32_t *width, uint32_t *height,
				 uint32_t *pitch);
void wgvk_device_set_retire_hook(VkDevice device, void (*fn)(void *data), void *data);
```

`wgvk_image_present_texture` copies the latest texels back from a
view-format alias first (`wgvk_image.current`), on the device's queue.

**The page.** `rvgpu_present_grab(res)` (exported) → four words {texture
handle (`Module.rvgpuJsObject`), width, height, flags} or 0. rvgpu-worker.js
(section 8's messages, plus):

- page → worker `{type:"table", address, slots, slotSize, header, gpuPid}`:
  the Worker watches the surface table every 2 ms; a format-2 slot whose
  `gpu_serial` `done` has passed and whose present it has not drawn is
  drawn: the texture into its OffscreenCanvas (a `webgpu` context, the
  preferred canvas format; one triangle that loads each texel: flips unless
  `Y_0_TOP`, any float colour format, sRGB through an sRGB view), then
  `transferToImageBitmap`, posted as `{type:"present", ctx, res, frame,
  bitmap, w, h, grabbed:true, bgra:false, blitMs, stats}`.
  `{type:"grab", ctx, res, present}` (display.js asking) does the same.
- page → worker `{type:"imported", res, frame}` (new; gpu-exec.js sends it
  to both Workers, vrend's ignores it): the page took that bitmap. **One
  frame in flight per resource**: a newer frame waits until then (or 250 ms)
  and only the newest waiting one is drawn.
- `{type:"ready", ..., fallback:true}`: no WebGPU device in this browser,
  said before the ring is touched; gpu-exec.js then starts vrend's Worker on
  the same ring (`?exec=webgpu` also falls back when `navigator.gpu` is
  missing on the page).

**Venus WSI needs from wgvk** (what Mesa 25.0.7's `vn_physical_device.c`
checks before it gives guests VK_KHR_swapchain over vtest; implemented, and
checked by `tests/wsi.test`): `VK_EXT_external_memory_dma_buf` with image
and buffer memory exportable as dma-buf/opaque fd, the LINEAR DRM modifier
with the optimal (colour attachment) features for 2D colour formats (the
image is a texture; its linear layout is only reported), dedicated
exportable memory for such an image, and binary semaphores that import and
export sync files (fd -1 = signalled: what venus' WSI imports through vkr).
wgvk hands out no fds (no vkGetMemoryFdKHR): vkr makes the blob itself.

**wgvk's own WSI** (`vulkan/wgvk_wsi.c`, native builds only): Mesa's common
WSI with VK_KHR_surface, VK_EXT_headless_surface, VK_KHR_swapchain and, since
`build/native.sh` builds `-Dplatforms=x11,wayland` (`GPU_PLATFORMS`, default
`x11,wayland`; empty for none; an existing build is reconfigured),
VK_KHR_xcb_surface, VK_KHR_xlib_surface and VK_KHR_wayland_surface: software
WSI (CPU images, a buffer blit at present into memory the WSI imports with
VK_EXT_external_memory_host, now implemented: MIT-SHM on X11, wl_shm on
Wayland). For T2's report: zink's kopper on X11 (the chroot's Mesa 25.0.7)
now gets its surface: `glxinfo -B` runs, es2gears_x11 draws (~1500 frames/s
under Xvfb), Blender's UI gets past its window (then stops on a
`vk_format_get_aspect_format` assertion, a depth/stencil copy: T2's).
glxgears dies in a geometry shader compile (`VARYING_SLOT_PRIMITIVE_ID`
output, T4/T3b). Nothing in the page creates swapchains on wgvk.

**For T7, from Blender on wgvk natively (T2, 2026-10-10).** zink on X11
(GLX or EGL, also with `LIBGL_ALWAYS_SOFTWARE=1` and
`LIBGL_KOPPER_DISABLE=1`) always presents through kopper, which calls
`vkCreateXcbSurfaceKHR` without checking the extension: with wgvk (no X11
surface) Blender's UI and `glxinfo` crash at the first window (a call through
a NULL pointer in libgallium, `zink_kopper`). Blender's UI and its Workbench
viewport under Xvfb need wgvk to offer `VK_KHR_xcb_surface` and
`VK_KHR_xlib_surface` natively: Mesa's common WSI with its X11 backend (the
native build's `-Dplatforms=` is empty today) on top of your swapchain. The
headless renders (Workbench, EEVEE) do not present and work now.

## 10. Build and test entry points (T0)

Everything runs in the build VM, which mounts only the main checkout:
`gpu/webgpu/build/vm.sh SCRIPT [ARGS]` (on the Mac) copies this worktree to
`~/wt/NAME` in the VM (rsync; NAME = the worktree's directory name, or
`$GPU_BUILD`) and runs SCRIPT there. Variables passed along: `NO_BUILD`,
`NO_LINK`, `GPU_NATIVE`, `WGVK_*`.

- `build/native.sh [ninja targets]`: the native Mesa build, incremental,
  in `~/rvwasm/webgpu/b/NAME/native`. `GPU_NATIVE=full` (default for the
  integration build `webgpu`): wgvk ICD, zink, llvmpipe + EGL surfaceless,
  lavapipe, mesa_clc and vtn_bindgen2 (about 850 MB). `GPU_NATIVE=min`
  (default for track builds): wgvk only (about 270 MB). `GPU_NATIVE=zink`:
  wgvk + zink + EGL (T6). Changing the profile needs a fresh `$B/native`.
- `build/wasm.sh [ninja targets]`: the Emscripten build of the same tree,
  `$B/wasm`; default targets `src/webgpu/rvgpu/rvgpu.js` (rvgpu.js +
  rvgpu.wasm, linked by meson: wgvk + rvgpu, JSPI, emdawnwebgpu's port at
  the link) and zink's libzink.a. Front ends add their libraries to
  `rvgpu_deps`, their exports to `rvgpu_exports` and waiting exports to
  `rvgpu_jspi_exports` in `mesa/src/webgpu/rvgpu/meson.build`.
- `build/check.sh [TEST...]`: native.sh, then every `gpu/webgpu/tests/*.test`
  (an executable script printing `ok NAME: ...` or `FAIL NAME: ...`, exit
  status non-zero on failure). Environment: `WT TREE B W T WGVK_ICD
  LVP_ICD` (lavapipe from the integration build when the track's build has
  none). Today: `icd` (vulkaninfo lists wgvk; the P4 oracle), `ring` (the
  ring consumer), `smoke-clear` (the clear smoke test through the ICD),
  `objects` (T1's objects through the API and `wgvk-objects-internal`),
  `zink` (eglinfo on zink on wgvk; skipped without a zink build), `wsi`
  (T7: wgvk's headless swapchain and venus' WSI checks; vkcube on Xvfb and
  on weston when the VM has them: `apt-get install xvfb x11-apps weston
  vulkan-tools`, plus the X11/Wayland -dev packages native.sh's platforms need).
- `build/smoke-page.sh [--no-build]` (on the Mac): wasm.sh, then the page
  smoke test in the test browser (`tools/with-browser.sh`; a server of its
  own with COOP/COEP, not :8000). Screenshot in `build/rvgpu-page/smoke.png`.
- Per-build trees: `build/tree.sh` makes `~/rvwasm/webgpu/trees/NAME` (a git
  worktree of the pinned clone `~/rvwasm/webgpu/mesa-main`, with
  `patches/mesa-page.diff` applied and `src/webgpu` linked to this
  worktree's copy). The patch is re-applied (the tree reset) whenever it
  changes. Edit Mesa files in that tree, then `build/mkpatch.sh` writes the
  diff back into the VM copy's patches/ (copy it to the Mac worktree).
- `profiles/p4-lavapipe.json`: vulkaninfo --json of lavapipe under wgmask with
  `spikes/wgmask/p4-env.sh` (the profile zink + Blender 4.3 ran on).
  `profiles/check-profile.py [--allow A,B] [--limits wgvk-limits.txt] P4 DEVICE`
  checks extensions, true features and max*/min*Alignment limits; the
  limits in `wgvk-limits.txt` are WebGPU's honest numbers (nothing emulates
  more), checked against a floor instead of P4. `profiles/gen-p4.py` writes
  `vulkan/wgvk_p4.c` from it (what the skeleton advertises).
- Disk: each track costs a Mesa tree (about 420 MB) plus its builds. Keep
  15 GB free in the VM (`df -h /`); remove a finished track's
  `trees/NAME` (`git -C ~/rvwasm/webgpu/mesa-main worktree remove`) and
  `b/NAME`.
