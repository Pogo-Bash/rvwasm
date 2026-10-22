# rvgpu: the page host (rvgpu.js + rvgpu.wasm, Emscripten only): the ring, guest memory, the device handed to
# wgvk, the smoke test. gpu/webgpu/build/wasm.sh builds it; the cross file names emdawnwebgpu's port file in
# the property emdawnwebgpu_port. Front ends (vfront, vkr) add their libraries to rvgpu_deps.

rvgpu_port = meson.get_external_property('emdawnwebgpu_port')
rvgpu_deps = [idep_wgvk]
rvgpu_exports = ['_malloc', '_free', '_rvgpu_init', '_rvgpu_ring_attach', '_rvgpu_ring_drain',
                 '_rvgpu_smoke_clear', '_rvgpu_smoke_draw', '_rvgpu_pipeline_cache', '_rvgpu_poll', '_rvgpu_smoke_stats', '_rvgpu_smoke_msg', '_rvgpu_smoke_device', '_rvgpu_smoke_texture',
                 '_rvgpu_present_grab']
# Exports that may wait for the GPU (wgpuInstanceWaitAny suspends them with JSPI): they return Promises.
rvgpu_jspi_exports = ['rvgpu_ring_drain', 'rvgpu_smoke_clear', 'rvgpu_smoke_draw']

# vfront (T6): the virgl ring ops on zink, started by the Worker with vfront_rvgpu_start()
rvgpu_runtime_methods = ['UTF8ToString', 'HEAPU8', 'HEAPU32']
if is_variable('idep_vfront_rvgpu')
  rvgpu_deps += idep_vfront_rvgpu
  rvgpu_exports += ['_vfront_rvgpu_start', '_vfront_replay_page']
  rvgpu_jspi_exports += ['vfront_replay_page', 'vfront_rvgpu_start']
  rvgpu_runtime_methods += 'FS'   # (the capture replay test writes the capture into the module's files)
endif

# T5: vkr, the venus front end (src/webgpu/vkr), when this build has its sources
if is_variable('idep_vkr')
  rvgpu_deps += idep_vkr
  rvgpu_exports += ['_rvgpu_vkr_page_start', '_rvgpu_poll_ms', '_rvgpu_debug_where', '_rvgpu_debug_words_at']
endif

rvgpu = executable(
  'rvgpu',
  files('rvgpu.c', 'rvgpu_page.c', '../tests/smoke-clear.c', '../tests/smoke-draw.c'),
  c_args : ['-DSMOKE_NO_MAIN'],
  include_directories : [inc_include, inc_src, inc_webgpu, include_directories('.', '../tests')],
  dependencies : rvgpu_deps,
  link_language : 'cpp',
  link_args : [
    '--use-port=' + rvgpu_port,
    '-sJSPI', '-sJSPI_EXPORTS=' + ','.join(rvgpu_jspi_exports),
    '-sMODULARIZE', '-sEXPORT_NAME=RvgpuModule', '-sENVIRONMENT=web,worker',
    '-sALLOW_MEMORY_GROWTH', '-sSTACK_SIZE=1MB',
    # Mesa is compiled with -pthread (meson's threads dependency), so the module has shared memory, but wgvk
    # starts no threads (INTERFACES.md 2): no pool of pthread Workers at start-up.
    '-sPTHREAD_POOL_SIZE=0',
    # and a thread anything starts anyway fails loudly instead of waiting for a Worker that never comes
    # (INTERFACES.md 7.3: zink's queues run inline, MESA_UTIL_QUEUE_INLINE)
    '-sPTHREAD_POOL_SIZE_STRICT=2',
    '-sEXPORTED_FUNCTIONS=' + ','.join(rvgpu_exports),
    '-sEXPORTED_RUNTIME_METHODS=' + ','.join(rvgpu_runtime_methods),
    '--js-library', meson.current_source_dir() / 'library_rvgpu.js',
  ],
  link_depends : files('library_rvgpu.js'),
  name_suffix : 'js',
)
