#!/bin/bash
# inside the chroot: bash /out/wgmask/run-profile.sh PROFILE (p0|p1|full) [CMD]
p=$1; shift
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1
if [ $p != full ]; then
	export VK_LAYER_PATH=/out/wgmask VK_INSTANCE_LAYERS=VK_LAYER_RVWASM_wgmask WGMASK_EXTS=/out/wgmask/$p-exts.txt \
		WGMASK_FEATS=/out/wgmask/$p-feats.txt WGMASK_API=${WGMASK_API:-1.1} WGMASK_LIMITS=1 WGMASK_LOG=1
fi
"${@:-eglinfo -p surfaceless}"
