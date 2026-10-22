# T5: vkr (virglrenderer's venus renderer, src/venus) built into the page module on wgvk
# (gpu/webgpu/INTERFACES.md 7). Its sources are this build's patched virglrenderer tree, which
# gpu/webgpu/build/vkr-tree.sh links into the Mesa tree as src/vkr-src; without that link there is no vkr.
fs = import('fs')
vkr_link = meson.project_source_root() / 'src' / 'vkr-src'
with_vkr = fs.is_dir(vkr_link / 'src' / 'venus')
if with_vkr
  # (the link's target: meson refuses absolute paths that look inside the source tree)
  vkr_src = run_command('readlink', '-f', vkr_link, check : true).stdout().strip()
endif
if with_vkr
  vkr_venus = vkr_src / 'src' / 'venus'
  vkr_files = []
  foreach f : ['vkr_acceleration_structure', 'vkr_buffer', 'vkr_command_buffer', 'vkr_common', 'vkr_context',
               'vkr_cs', 'vkr_descriptor_heap', 'vkr_descriptor_set', 'vkr_device', 'vkr_device_memory',
               'vkr_host_copy', 'vkr_image', 'vkr_instance', 'vkr_library', 'vkr_physical_device', 'vkr_pipeline',
               'vkr_query_pool', 'vkr_queue', 'vkr_render_pass', 'vkr_renderer', 'vkr_ring', 'vkr_transport']
    vkr_files += vkr_venus / f + '.c'
  endforeach
  vkr_files += vkr_src / 'src' / 'virgl_util.c'
  # (order: our config.h, then the venus protocol's own Vulkan headers before Mesa's include/)
  inc_vkr = include_directories('include', vkr_src / 'gen', vkr_venus, vkr_src / 'src')
  vkr_c_args = ['-DHAVE_CONFIG_H=1', '-DVIRTGPU_DRM_CAPSET_VENUS=4', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
                '-Wno-sign-compare', '-Wno-pedantic', '-Wno-unused-function', '-Wno-declaration-after-statement',
                '-Wno-missing-prototypes', '-Wno-unused-variable']
  libvkr = static_library(
    'vkr',
    vkr_files, files('rvgpu_vkr.c'),
    include_directories : [inc_vkr, inc_src, inc_include, inc_webgpu],
    c_args : vkr_c_args,
    dependencies : [idep_mesautil, dep_thread, dep_webgpu, idep_vulkan_util_headers],
    gnu_symbol_visibility : 'hidden',
  )
  idep_vkr = declare_dependency(link_with : libvkr, include_directories : include_directories('.'))
endif

if with_vkr and host_machine.system() != 'emscripten'
  # The native test bed: gpuserver's venus half and the page's executor in one process (vkr-vtest.c).
  executable(
    'vkr-vtest',
    ['vkr-vtest.c', '../rvgpu/rvgpu.c', '../rvgpu/rvgpu_native.c'],
    include_directories : [inc_include, inc_src, inc_webgpu, include_directories('.', '../rvgpu')],
    dependencies : [idep_vkr, idep_wgvk, idep_mesautil, dep_webgpu, dep_thread],
    c_args : ['-Wno-missing-field-initializers'],
    link_language : 'cpp',
    build_by_default : true,
  )
endif
