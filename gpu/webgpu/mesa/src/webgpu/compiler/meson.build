# T3: wgvk's shader compiler: NIR legalisation passes (T3b) and NIR -> WGSL (T3a).
# libwgvk_compiler is linked by wgvk (vulkan/); the tools below are native-only test drivers.

wgvk_compiler_files = files(
  'nir_to_wgsl.c',
  'nir_to_wgsl.h',
  'wgsl_nir.c',
  'wgvk_compile.c',
  'wgvk_compile.h',
  'wgvk_legalize.c',
  'wgvk_legalize.h',
  '../poly/wgvk_poly.c',
  '../poly/wgvk_poly.h',
)

# Mesa's soft-fp64 library (float64.glsl) as SPIR-V, for shaders with 64-bit floats (as anv and turnip do).
wgvk_compiler_c_args = []
if prog_glslang.found()
  wgvk_compiler_files += custom_target(
    'float64_spv.h',
    input : [glsl2spirv, float64_glsl_file],
    output : 'float64_spv.h',
    command : [prog_python, '@INPUT@', '@OUTPUT@', prog_glslang, '--vn', 'float64_spv_source',
               '--glsl-version', '450', '-Olib', glslang_depfile],
    depfile : 'float64_spv.h.d',
  )
  wgvk_compiler_c_args += '-DWGVK_HAVE_SOFTFP64'
endif

libwgvk_compiler = static_library(
  'wgvk_compiler',
  wgvk_compiler_files,
  include_directories : [inc_include, inc_src, inc_webgpu, include_directories('.', '../poly')],
  dependencies : [idep_nir, idep_vtn, idep_mesautil, idep_wgvk_libpoly],
  c_args : ['-Wno-missing-field-initializers'] + wgvk_compiler_c_args,
  gnu_symbol_visibility : 'hidden',
)

idep_wgvk_compiler = declare_dependency(
  link_with : libwgvk_compiler,
  include_directories : include_directories('.', '../poly'),
  dependencies : [idep_nir, idep_vtn, idep_mesautil, idep_wgvk_libpoly],
)

if host_machine.system() == 'emscripten'
  # The same corpus driver in wasm under node (files through NODERAWFS): the compiler and the passes as the
  # page runs them, timed. node wgsl-corpus.js [options] FILE.spv...
  executable(
    'wgsl-corpus',
    'tools/wgsl-corpus.c',
    include_directories : [inc_include, inc_src, inc_webgpu],
    dependencies : [idep_wgvk_compiler],
    link_args : ['-sNODERAWFS=1', '-sALLOW_MEMORY_GROWTH=1', '-sSTACK_SIZE=4MB', '-sEXIT_RUNTIME=1'],
    build_by_default : false,
  )
else
  # SPIR-V modules -> WGSL files (+ tagged reasons for what blocks a module), for checking with Tint.
  executable(
    'wgsl-corpus',
    ['tools/wgsl-corpus.c', '../vulkan/wgvk_remap.c'],
    include_directories : [inc_include, inc_src, inc_webgpu],
    dependencies : [idep_wgvk_compiler],
    build_by_default : false,
  )
  # One compute shader as SPIR-V on Vulkan (lavapipe) and as wgsl-corpus's WGSL on Dawn native, compared.
  # No Mesa in it: Mesa's static SPIRV-Tools would interpose the copy inside libwebgpu_dawn.so.
  dep_cs_webgpu = dependency('webgpu_dawn', required : false)
  dep_cs_vulkan = dependency('vulkan', required : false)
  if dep_cs_webgpu.found() and dep_cs_vulkan.found()
    executable(
      'cs-run',
      'tools/cs-run.c',
      dependencies : [dep_cs_webgpu, dep_cs_vulkan, dep_m],
      build_by_default : false,
    )
    # T3b: a scene (tools/scene.h) drawn on Vulkan with its real vertex input and as legalised WGSL on Dawn.
    executable(
      'scene-run',
      'tools/scene-run.c',
      include_directories : [inc_include, inc_src, include_directories('../poly')],
      dependencies : [dep_cs_webgpu, dep_cs_vulkan, dep_m],
      build_by_default : false,
    )
    # A vertex + fragment pair rendered on Vulkan (lavapipe) and, as our WGSL, on Dawn native, compared.
    executable(
      'draw-run',
      'tools/draw-run.c',
      dependencies : [dep_cs_webgpu, dep_cs_vulkan, dep_m],
      build_by_default : false,
    )
  endif
endif
