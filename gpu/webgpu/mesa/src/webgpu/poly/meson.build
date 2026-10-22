# T4: Mesa's libpoly (src/poly: geometry shaders, tessellation and transform feedback emulated in compute)
# built for wgvk, without enabling a driver that uses it (with_poly is false for webgpu): its OpenCL helpers
# are compiled by mesa_clc to SPIR-V and turned into NIR builder code by vtn_bindgen2, both run natively.
# A track build (GPU_NATIVE=min, no LLVM) and the Emscripten build cannot build those tools, so they come
# from this tree's own build when it has them, else from the integration build's native build
# ($W/b/webgpu/native, which GPU_NATIVE=full builds), found next to this build's directory.
poly_src = meson.project_source_root() / 'src' / 'poly'
_tools = meson.global_build_root() / '..' / '..' / 'webgpu' / 'native' / 'src' / 'compiler'
if is_variable('prog_mesa_clc') and is_variable('prog_vtn_bindgen2')
  wgvk_mesa_clc = prog_mesa_clc
  wgvk_vtn_bindgen2 = prog_vtn_bindgen2
else
  wgvk_mesa_clc = find_program('mesa_clc', dirs : [_tools / 'clc'], native : true)
  wgvk_vtn_bindgen2 = find_program('vtn_bindgen2', dirs : [_tools / 'spirv'], native : true)
endif

wgvk_libpoly_cl = files(poly_src / 'cl' / 'geometry.cl', poly_src / 'cl' / 'tessellation.cl')
wgvk_libpoly_spv = custom_target(
  'wgvk_libpoly.spv',
  input : wgvk_libpoly_cl,
  output : 'libpoly.spv',
  command : [
    wgvk_mesa_clc, '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', '@INPUT@', '--',
    '-I' + (meson.project_source_root() / 'include'),
    '-I' + (meson.project_source_root() / 'src' / 'compiler' / 'libcl'),
    '-I' + (poly_src / 'cl'),
    '-I' + (meson.project_source_root() / 'src'),
    cl_args,
  ],
  depfile : 'libpoly.spv.d',
)
# wgvk's own helpers on libpoly (indirect GS draw setup)
wgvk_poly_cl_spv = custom_target(
  'wgvk_poly_cl.spv',
  input : files('wgvk_poly.cl'),
  output : 'wgvk_poly_cl.spv',
  command : [
    wgvk_mesa_clc, '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', '@INPUT@', '--',
    '-I' + (meson.project_source_root() / 'include'),
    '-I' + (meson.project_source_root() / 'src' / 'compiler' / 'libcl'),
    '-I' + (poly_src / 'cl'),
    '-I' + (meson.project_source_root() / 'src'),
    '-I' + meson.current_source_dir(),
    cl_args,
  ],
  depfile : 'wgvk_poly_cl.spv.d',
)
# the tessellator: SPIR-V embedded as data, imported at run time (see wgvk_tess.cl)
wgvk_tess_spv = custom_target(
  'wgvk_tess.spv',
  input : files('wgvk_tess.cl'),
  output : 'wgvk_tess.spv',
  command : [
    wgvk_mesa_clc, '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', '@INPUT@', '--',
    '-I' + (meson.project_source_root() / 'include'),
    '-I' + (meson.project_source_root() / 'src' / 'compiler' / 'libcl'),
    '-I' + (poly_src / 'cl'),
    '-I' + (meson.project_source_root() / 'src'),
    '-I' + meson.current_source_dir(),
    cl_args,
  ],
  depfile : 'wgvk_tess.spv.d',
)
wgvk_tess_spv_h = custom_target(
  'wgvk_tess_spv.h',
  input : [files_xxd, wgvk_tess_spv],
  output : 'wgvk_tess_spv.h',
  command : [prog_python, '@INPUT@', '@OUTPUT@', '-n', 'wgvk_tess_spv', '-b'],
)

wgvk_poly_cl_gen = custom_target(
  'wgvk_poly_cl_gen',
  input : wgvk_poly_cl_spv,
  output : ['wgvk_poly_cl.cpp', 'wgvk_poly_cl.h'],
  command : [wgvk_vtn_bindgen2, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'],
)

# (the generated header is included as "poly/cl/libpoly.h": it is made in a directory of that name here)
subdir('poly/cl')

wgvk_libpoly = static_library(
  'wgvk_libpoly',
  files(poly_src / 'nir' / 'poly_nir_lower_gs.c', poly_src / 'nir' / 'poly_nir_lower_sysvals.c',
        poly_src / 'nir' / 'poly_nir_lower_tess.c', poly_src / 'nir' / 'poly_nir_lower_vs.c',
        poly_src / 'nir' / 'poly_nir_passthrough_gs.c'),
  wgvk_libpoly_gen, wgvk_poly_cl_gen, wgvk_tess_spv_h,
  include_directories : [inc_include, inc_src, include_directories('.')],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  cpp_args : ['-Wno-missing-field-initializers'],
  gnu_symbol_visibility : 'hidden',
  dependencies : [idep_nir, idep_mesautil],
)
idep_wgvk_libpoly = declare_dependency(
  link_with : wgvk_libpoly,
  sources : [wgvk_libpoly_gen[1], wgvk_poly_cl_gen[1], wgvk_tess_spv_h],
  include_directories : include_directories('.'),
)
