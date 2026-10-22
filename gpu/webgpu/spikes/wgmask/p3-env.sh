# P3: what a first WebGPU-backed driver would offer (no geometry/tessellation/transform feedback, no fp64/int64,
# one viewport), WebGPU's limits except 16 uniform buffers per stage, and Mesa told to say GL 4.3 anyway.
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1
export VK_LAYER_PATH=/out/wgmask VK_INSTANCE_LAYERS=VK_LAYER_RVWASM_wgmask WGMASK_EXTS=/out/wgmask/p3-exts.txt \
	WGMASK_FEATS=/out/wgmask/p3-feats.txt WGMASK_API=1.1 WGMASK_UBOS=16 \
	WGMASK_LIMITS=sets,samplers,ssbo,textures,images,rts,vattrs,shared,invocations,uborange,lodbias,samples,lines,ubos,viewports
