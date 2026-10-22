#!/bin/bash
# (In the VM) T5: T2's t2-test (the same Vulkan program on lavapipe and on wgvk, outputs compared) through venus:
# the client in the trixie arm64 chroot -> vkr-vtest (vkr on wgvk on Dawn on lavapipe), compared with lavapipe
# directly. Needs a tree with T2's tests (mesa/src/webgpu/tests/t2). Arguments: t2-test case names.
. "$(dirname "$0")/../../build/env.sh"
S=$WT/gpu/webgpu/mesa/src/webgpu/tests/t2
LVP_ICD=$W/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
O=$W/out/t5/t2; mkdir -p $O/spv
for f in $S/*.vert $S/*.frag $S/*.comp $S/*.geom; do
	glslangValidator -V --target-env vulkan1.1 -o $O/spv/$(basename $f).spv $f > $O/glslang.log 2>&1 || { cat $O/glslang.log; exit 1; }
done
cc -O1 -g -Wall -Wno-missing-braces -I$TREE/include -o $O/t2-test $S/t2-test.c -lvulkan || exit 1
rm -rf $O/lvp $O/venus
VK_DRIVER_FILES=$LVP_ICD timeout 120 $O/t2-test $O/lvp $O/spv "$@" > $O/lvp.log 2>&1 || { tail -20 $O/lvp.log; echo "lavapipe run failed"; exit 1; }
TAIL=${TAIL:-40} STATS=1 bash $(dirname "$0")/native-venus.sh "/out/t5/t2/t2-test /out/t5/t2/venus /out/t5/t2/spv $* > /out/t5/t2/venus.log 2>&1; echo t2-test rc \$?; tail -15 /out/t5/t2/venus.log"
$O/t2-test --compare $O/lvp $O/venus "$@"
