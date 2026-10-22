// rvgpu in the page: the machine's memory as seen from rvgpu.wasm (INTERFACES.md 3). The machine's memory is a
// separate, shared WebAssembly.Memory (memory64); C cannot point into it, so these functions copy, or hand
// WebGPU a view straight over it (queue.writeBuffer / writeTexture take shared views: no copy in between).
// The page sets Module.rvgpuMemory (that WebAssembly.Memory, or a SharedArrayBuffer for tests) and
// Module.rvgpuDevice (the Worker's GPUDevice) before calling _rvgpu_init().
addToLibrary({
  $RVGPU: {
    buf: null,
    u8: null,
    buffer() {
      const m = Module.rvgpuMemory;
      const b = m instanceof WebAssembly.Memory ? m.buffer : m;
      if (b !== RVGPU.buf) {
        RVGPU.buf = b;
        RVGPU.u8 = new Uint8Array(b);
      }
      return b;
    },
    i32(addr) {
      return new Int32Array(RVGPU.buffer(), addr, 1);
    },
  },

  // (Module.rvgpuJsObject(ptr): the JavaScript object of a webgpu.h handle, for rvgpu-worker.js)
  rvgpu_js_import_device__postset: 'Module.rvgpuJsObject = (p) => WebGPU.getJsObject(p);',
  rvgpu_js_import_device__deps: ['$WebGPU'],
  rvgpu_js_import_device: (instance) => Module.rvgpuDevice ? WebGPU.importJsDevice(Module.rvgpuDevice, instance) : 0,

  // (T2) a pipeline cache record for IndexedDB: copied out of the heap now, stored by the Worker later
  rvgpu_js_cache_store__sig: 'vpp',
  rvgpu_js_cache_store: (rec, size) => {
    if (Module.rvgpuCacheStore) Module.rvgpuCacheStore(HEAPU8.slice(rec, rec + size));
  },

  rvgpu_guest_load32__deps: ['$RVGPU'],
  rvgpu_guest_load32__i53abi: true,
  rvgpu_guest_load32__sig: 'ij',
  rvgpu_guest_load32: (addr) => Atomics.load(RVGPU.i32(addr), 0),

  rvgpu_guest_store32__deps: ['$RVGPU'],
  rvgpu_guest_store32__i53abi: true,
  rvgpu_guest_store32__sig: 'vji',
  rvgpu_guest_store32: (addr, v) => {
    const a = RVGPU.i32(addr);
    Atomics.store(a, 0, v);
    Atomics.notify(a, 0);
  },

  rvgpu_guest_read__deps: ['$RVGPU'],
  rvgpu_guest_read__i53abi: true,
  rvgpu_guest_read__sig: 'vpjp',
  rvgpu_guest_read: (dst, addr, len) => {
    RVGPU.buffer();
    HEAPU8.set(RVGPU.u8.subarray(addr, addr + len), dst);
  },

  rvgpu_guest_write__deps: ['$RVGPU'],
  rvgpu_guest_write__i53abi: true,
  rvgpu_guest_write__sig: 'vjpp',
  rvgpu_guest_write: (addr, src, len) => {
    RVGPU.buffer();
    RVGPU.u8.set(HEAPU8.subarray(src, src + len), addr);
  },

  rvgpu_guest_upload_buffer__deps: ['$RVGPU', '$WebGPU'],
  rvgpu_guest_upload_buffer__i53abi: true,
  rvgpu_guest_upload_buffer__sig: 'vppjjp',
  rvgpu_guest_upload_buffer: (q, b, boff, addr, len) => {
    WebGPU.getJsObject(q).writeBuffer(WebGPU.getJsObject(b), boff, new Uint8Array(RVGPU.buffer(), addr, len));
  },

  rvgpu_guest_upload_texture__deps: ['$RVGPU', '$WebGPU'],
  rvgpu_guest_upload_texture__i53abi: true,
  rvgpu_guest_upload_texture__sig: 'vppjpp',
  rvgpu_guest_upload_texture: (q, dst, addr, layout, size) => {
    // WGPUTexelCopyTextureInfo {texture, mipLevel, origin{x,y,z}, aspect}; WGPUTexelCopyBufferLayout {u64 offset,
    // u32 bytesPerRow, u32 rowsPerImage}; WGPUExtent3D {w, h, depth}
    const tex = WebGPU.getJsObject(HEAPU32[dst >> 2]);
    const d = { texture: tex, mipLevel: HEAPU32[(dst >> 2) + 1],
                origin: { x: HEAPU32[(dst >> 2) + 2], y: HEAPU32[(dst >> 2) + 3], z: HEAPU32[(dst >> 2) + 4] } };
    const off = Number(HEAPU64[layout >> 3]);
    const l = { offset: 0, bytesPerRow: HEAPU32[(layout >> 2) + 2], rowsPerImage: HEAPU32[(layout >> 2) + 3] };
    const s = { width: HEAPU32[size >> 2], height: HEAPU32[(size >> 2) + 1], depthOrArrayLayers: HEAPU32[(size >> 2) + 2] };
    const b = RVGPU.buffer();
    WebGPU.getJsObject(q).writeTexture(d, new Uint8Array(b, addr + off, b.byteLength - addr - off), l, s);
  },

  // (the buffer must not also be mapped from C: one getMappedRange per range)
  rvgpu_guest_from_mapped__deps: ['$RVGPU', '$WebGPU'],
  rvgpu_guest_from_mapped__i53abi: true,
  rvgpu_guest_from_mapped__sig: 'vpjjp',
  rvgpu_guest_from_mapped: (mapped, boff, addr, len) => {
    const r = WebGPU.getJsObject(mapped).getMappedRange(boff, len);
    RVGPU.buffer();
    RVGPU.u8.set(new Uint8Array(r), addr);
  },

  rvgpu_guest_ptr: (addr, len) => 0,
  rvgpu_guest_set_host: (base, size) => {},
});
