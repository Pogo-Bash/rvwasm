# libpoly.cpp / libpoly.h from vtn_bindgen2, in a directory poly/cl so that "poly/cl/libpoly.h" resolves.
wgvk_libpoly_gen = custom_target(
  'wgvk_libpoly_gen',
  input : wgvk_libpoly_spv,
  output : ['libpoly.cpp', 'libpoly.h'],
  command : [wgvk_vtn_bindgen2, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'],
)
