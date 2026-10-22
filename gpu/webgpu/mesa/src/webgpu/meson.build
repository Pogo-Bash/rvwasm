# wgvk and our other page-side Mesa code (gpu/webgpu/INTERFACES.md). This directory lives in the rvwasm
# repository (gpu/webgpu/mesa/src/webgpu) and is linked into the pinned Mesa tree as src/webgpu.
inc_webgpu = include_directories('include', 'rvgpu')
subdir('poly')
subdir('compiler')
subdir('vulkan')
subdir('vkr')
# vfront and rvgpu come after src/gallium (src/meson.build, patches/mesa-page.diff): rvgpu links vfront, which
# needs zink.
