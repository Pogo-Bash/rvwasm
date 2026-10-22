#!/bin/bash
# (In the VM) T5: vkr-bench (gpu/webgpu/vkr/tests) on lavapipe, on wgvk directly, and through venus
# (chroot client -> vkr-vtest -> vkr -> wgvk -> Dawn -> lavapipe). Arguments: N ROUNDS.
. "$(dirname "$0")/../../build/env.sh"
T=$WT/gpu/webgpu/vkr/tests
LVP_ICD=$W/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
WGVK_ICD=$B/native/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json
O=$W/out/t5/bench; mkdir -p $O
glslangValidator -V --target-env vulkan1.1 -o $O/add.spv $T/add.comp > /dev/null || exit 1
cc -O2 -Wall -Wno-missing-braces -I$TREE/include -o $O/vkr-bench $T/vkr-bench.c -lvulkan || exit 1
echo "== lavapipe"; VK_DRIVER_FILES=$LVP_ICD timeout 120 $O/vkr-bench $O/add.spv "$@"
echo "== wgvk"; VK_DRIVER_FILES=$WGVK_ICD WGVK_DAWN_DRIVER_FILES=$LVP_ICD timeout 120 $O/vkr-bench $O/add.spv "$@"
echo "== venus"; STAIL=4 TAIL=30 bash $(dirname "$0")/native-venus.sh "/out/t5/bench/vkr-bench /out/t5/bench/add.spv $*"
