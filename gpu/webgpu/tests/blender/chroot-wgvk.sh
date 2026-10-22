#!/bin/bash
# (VM) Run CMD in the trixie chroot with zink (Debian's Mesa 25.0.7) on wgvk (this worktree's build) on Dawn
# native on lavapipe (the chroot's). The host's ~/rvwasm is bind-mounted at the same path inside, so the ICD
# json and Dawn's rpaths hold.   chroot-wgvk.sh [lvp|lvp-main|wgvk|wgvk-main] 'CMD'
# wgvk-main: Dawn on the integration build's lavapipe (Mesa main, with LLVM 20 copied from the VM) instead of
# the chroot's 25.0.7, which blends no SNORM formats, so Dawn gives no texture-formats-tier1 (storage R16F,
# RG16Unorm, RG11B10, RGB10A2: EEVEE needs them; browsers have tier1)
drv=$1; shift
C=$HOME/rvwasm/webgpu/trixie
bash "$(dirname "$0")/../../spikes/vm/chroot-run.sh" true
sudo mkdir -p $C$HOME/rvwasm
mountpoint -q $C$HOME/rvwasm || sudo mount --bind $HOME/rvwasm $C$HOME/rvwasm
B=${WGVK_B:-$HOME/rvwasm/webgpu/b/gpu-t2/native}
envs="MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=4.3 MESA_GLSL_VERSION_OVERRIDE=430"
if [ "$drv" = lvp-main ]; then
	envs="$envs VK_ICD_FILENAMES=$HOME/rvwasm/webgpu/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json LD_LIBRARY_PATH=$HOME/rvwasm/webgpu/out/t2/llvm20"
elif [ "$drv" = wgvk-main ]; then
	envs="$envs VK_ICD_FILENAMES=$B/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json VK_DRIVER_FILES=$B/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json WGVK_DAWN_DRIVER_FILES=$HOME/rvwasm/webgpu/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json WGVK_DEVICE_TYPE=cpu LD_LIBRARY_PATH=$HOME/rvwasm/webgpu/out/t2/llvm20"
elif [ "$drv" = wgvk ]; then
	envs="$envs VK_ICD_FILENAMES=$B/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json VK_DRIVER_FILES=$B/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json WGVK_DAWN_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json WGVK_DEVICE_TYPE=cpu"
else
	envs="$envs VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
fi
sudo chroot $C /usr/bin/env -i PATH=/usr/bin:/bin HOME=/root $envs ${EXTRA_ENV:-} bash -c "$1"
