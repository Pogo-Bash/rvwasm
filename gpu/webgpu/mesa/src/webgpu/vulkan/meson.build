# wgvk: Vulkan on WebGPU. Natively a Vulkan ICD (libvulkan_webgpu.so) on Dawn native; with Emscripten a
# static library that rvgpu.wasm links (gpu/webgpu/build/wasm.sh), on emdawnwebgpu (the cross file passes
# --use-port for it).

is_wasm = host_machine.system() == 'emscripten'
if is_wasm
  dep_webgpu = null_dep
else
  dep_webgpu = dependency('webgpu_dawn')
endif

wgvk_entrypoints = custom_target(
  'wgvk_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['wgvk_entrypoints.h', 'wgvk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'wgvk',
    '--beta', with_vulkan_beta.to_string()
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

wgvk_files = files(
  'wgvk_bo.h',
  'wgvk_cmd.c',
  'wgvk_copy.c',
  'wgvk_descriptor.c',
  'wgvk_descriptor_set.c',
  'wgvk_device.c',
  'wgvk_format.c',
  'wgvk_format.h',
  'wgvk_heap.c',
  'wgvk_image.c',
  'wgvk_index.c',
  'wgvk_instance.c',
  'wgvk_memory.c',
  'wgvk_meta.c',
  'wgvk_p4.c',
  'wgvk_physical_device.c',
  'wgvk_pipeline.c',
  'wgvk_present.c',
  'wgvk_private.h',
  'wgvk_query.c',
  'wgvk_query_cmd.c',
  'wgvk_remap.c',
  'wgvk_render.c',
  'wgvk_replay.h',
  'wgvk_sampler.c',
  'wgvk_shader.c',
  'wgvk_sync.c',
  'wgvk_wgpu.c',
  'wgvk_wsi.c',
)

wgvk_deps = [
  idep_wgvk_compiler,
  idep_nir,
  idep_mesautil,
  idep_vulkan_runtime,
  idep_vulkan_util,
  dep_webgpu,
]
# T7: Mesa's common WSI natively (headless surfaces and swapchains, wgvk_wsi.c); none in the page
wgvk_c_args = []
if not is_wasm
  wgvk_deps += idep_vulkan_wsi
  wgvk_c_args += '-DWGVK_WSI'
endif

libwgvk = static_library(
  'wgvk',
  [wgvk_files, wgvk_entrypoints],
  include_directories : [inc_include, inc_src, inc_webgpu],
  dependencies : wgvk_deps,
  c_args : ['-Wno-missing-field-initializers'] + wgvk_c_args,
  gnu_symbol_visibility : 'hidden',
)

# (libwgvk already holds the Vulkan runtime's objects, which it links whole; naming wgvk_deps again here
# would link them twice)
idep_wgvk = declare_dependency(
  link_whole : libwgvk,
  include_directories : inc_webgpu,
  dependencies : [dep_webgpu, idep_vulkan_runtime_headers, idep_vulkan_util_headers],
)

if not is_wasm
  libvulkan_webgpu = shared_library(
    'vulkan_webgpu',
    link_whole : [libwgvk],
    dependencies : [dep_webgpu],
    link_args : [ld_args_build_id, ld_args_bsymbolic, ld_args_gc_sections],
    gnu_symbol_visibility : 'hidden',
    install : true,
  )

  icd_file_name = libname_prefix + 'vulkan_webgpu.' + libname_suffix
  wgvk_icd = custom_target(
    'wgvk_devenv_icd',
    input : [vk_icd_gen, vk_api_xml],
    output : 'wgvk_devenv_icd.@0@.json'.format(host_machine.cpu()),
    command : [
      prog_python, '@INPUT0@',
      '--api-version', '1.1', '--xml', '@INPUT1@',
      '--icd-lib-path', meson.current_build_dir(),
      '--icd-filename', icd_file_name,
      '--out', '@OUTPUT@',
    ],
    build_by_default : true,
  )
  devenv.append('VK_DRIVER_FILES', wgvk_icd.full_path())
endif

if not is_wasm
  # T1's tests of wgvk's insides (bind groups and their caches), linked with libwgvk directly;
  # gpu/webgpu/tests/objects.test builds and runs it.
  executable(
    'wgvk-objects-internal',
    ['../tests/objects-internal.c', wgvk_entrypoints[0]],
    include_directories : [inc_include, inc_src, inc_webgpu, include_directories('.')],
    dependencies : [idep_wgvk, idep_wgvk_compiler, idep_vulkan_runtime_headers, idep_vulkan_util_headers,
                    idep_mesautil, idep_vulkan_wsi_headers],
    c_args : ['-Wno-missing-field-initializers'] + wgvk_c_args,
    build_by_default : false,
  )
endif
