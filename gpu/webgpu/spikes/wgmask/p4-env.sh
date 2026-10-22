# P4: P3 plus what Blender 4.3's GL backend insists on: geometry shaders (the driver would emulate them in
# compute), stores from vertex shaders (same path), 16 storage buffers per stage (bindings packed by the driver).
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1
export VK_LAYER_PATH=/out/wgmask VK_INSTANCE_LAYERS=VK_LAYER_RVWASM_wgmask WGMASK_EXTS=/out/wgmask/p4-exts.txt \
	WGMASK_FEATS=/out/wgmask/p4-feats.txt WGMASK_API=1.1 WGMASK_UBOS=16 WGMASK_SSBO=${WGMASK_SSBO:-24} \
	WGMASK_LIMITS=sets,samplers,ssbo,textures,images,rts,vattrs,shared,invocations,uborange,lodbias,samples,lines,ubos,viewports
