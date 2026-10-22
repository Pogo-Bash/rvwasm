# vfront: the virgl decoder on zink's Gallium (T6; gpu/webgpu/INTERFACES.md). Built after src/gallium (the
# src/meson.build hunk in patches/mesa-page.diff), natively and with Emscripten:
#   libvfront / idep_vfront   the decoder (vfront.h), on any Gallium screen (zink; softpipe natively when built)
#   libvfront_rvgpu           the page's front end on the ring (vf_rvgpu.c): rvgpu ops -> vfront, zink on wgvk
#   vfront-vtest, vfront-replay (native)   a vtest server around vfront, and capture replay (tools/)

inc_vfront = include_directories('.')
_vf_inc = [inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_gallium_drivers, inc_gallium_winsys,
           inc_virtio_gpu]
_vf_deps = [idep_nir, idep_mesautil]
_vf_link = [libgallium, libzink]
if with_gallium_softpipe
  _vf_deps += driver_softpipe
  _vf_link += libws_null
endif
if is_variable('_libgalliumvl_stub')
  _vf_link += _libgalliumvl_stub
elif is_variable('libgalliumvl_stub')
  _vf_link += libgalliumvl_stub
endif

libvfront = static_library(
  'vfront',
  files('vf_main.c', 'vf_decode.c', 'vf_shader.c', 'vf_caps.c', 'vf_formats.c', 'vf_tgsi_text.c', 'vf_ttn.c'),
  include_directories : _vf_inc,
  dependencies : _vf_deps,
  c_args : ['-Wno-missing-field-initializers'],
  gnu_symbol_visibility : 'hidden',
)

idep_vfront = declare_dependency(
  include_directories : [inc_vfront, inc_virtio_gpu],
  link_whole : [libvfront],
  link_with : _vf_link,
  dependencies : [idep_nir, idep_vtn, idep_mesautil, idep_xmlconfig, idep_vulkan_util, dep_thread, dep_dl,
                  dep_m] + (with_gallium_softpipe ? [driver_softpipe] : []),
)

if host_machine.system() == 'emscripten'
  libvfront_rvgpu = static_library(
    'vfront_rvgpu',
    files('vf_rvgpu.c', 'vf_present.c', 'tools/vfront_replay.c'),
    c_args : ['-DVF_REPLAY_MAIN=vfront_replay_main', '-DVF_REPLAY_PAGE'],
    # (headers only: a static library with idep_vfront would take libvfront's objects in a second time)
    include_directories : [inc_include, inc_src, inc_webgpu, inc_vfront, inc_virtio_gpu, inc_gallium,
                           inc_gallium_aux, inc_gallium_drivers, include_directories('tools')],
    dependencies : [idep_vulkan_util_headers],
    gnu_symbol_visibility : 'hidden',
  )
  idep_vfront_rvgpu = declare_dependency(link_whole : [libvfront_rvgpu], dependencies : [idep_vfront])
else
  # (static archives' symbols kept out of the executables' dynamic symbols: Mesa's SPIRV-Tools would otherwise
  # preempt the copy inside libwebgpu_dawn.so, whose own references go through the global scope, and Dawn's
  # SPIR-V validator crashed reading Mesa's version's tables: zink on wgvk, first pipeline)
  _vf_exe_link = ['-Wl,--exclude-libs,ALL']
  executable(
    'vfront-vtest',
    files('tools/vfront_vtest.c'),
    include_directories : [include_directories('tools')],
    dependencies : [idep_vfront],
    link_args : _vf_exe_link,
    install : false,
  )
  # the page's ring path natively: a capture through rvgpu's ring consumer into vf_rvgpu.c (tests/vfront.test)
  executable(
    'vfront-ring',
    files('tools/vfront_ring.c', 'vf_rvgpu.c', '../rvgpu/rvgpu.c', '../rvgpu/rvgpu_native.c'),
    c_args : ['-DVF_RVGPU_NATIVE'],
    include_directories : [include_directories('tools'), inc_include, inc_src, inc_webgpu],
    dependencies : [idep_vfront, dep_webgpu],
    link_args : _vf_exe_link,
    install : false,
  )
  executable(
    'vfront-replay',
    files('tools/vfront_replay.c'),
    include_directories : [include_directories('tools'), inc_include, inc_src, inc_gallium, inc_gallium_aux],
    dependencies : [idep_vfront],
    link_args : _vf_exe_link,
    install : false,
  )
endif
