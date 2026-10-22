#!/bin/bash
# inside the chroot: print the GL versions zink gives for one variant of the masks.
#   variant.sh NAME [VAR=VALUE ...]   (empty WGMASK_EXTS= means all extensions, WGMASK_FEATS=/dev/null no feature masked)
name=$1; shift
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1
export VK_LAYER_PATH=/out/wgmask VK_INSTANCE_LAYERS=VK_LAYER_RVWASM_wgmask WGMASK_EXTS=/out/wgmask/p1-exts.txt \
	WGMASK_FEATS=/out/wgmask/p1-feats.txt WGMASK_API=1.1 WGMASK_LIMITS=1
for kv in "$@"; do export "$kv"; done
[ -z "$WGMASK_EXTS" ] && unset WGMASK_EXTS
[ -z "$WGMASK_LIMITS" ] && unset WGMASK_LIMITS
v=$(eglinfo -p surfaceless 2>&1 | grep -E "profile version" | sed 's/OpenGL //; s/ Mesa.*//' | tr '\n' ';')
echo "$name: $v"
