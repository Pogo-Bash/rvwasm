#!/bin/bash
# (In the VM) T5: vkr-bench and its shader for the riscv64 guest (cross-compiled against the daily root's
# libvulkan), in $OUT (default ~/rvwasm/webgpu/out/t5/guest).
. "$(dirname "$0")/../../build/env.sh"
T=$WT/gpu/webgpu/vkr/tests
R=$HOME/rvwasm/debian-root-kde
OUT=${OUT:-$W/out/t5/guest}; mkdir -p $OUT
glslangValidator -V --target-env vulkan1.1 -o $OUT/add.spv $T/add.comp > /dev/null || exit 1
riscv64-linux-gnu-gcc -O2 -Wall -Wno-missing-braces -I$TREE/include -o $OUT/vkr-bench-rv64 $T/vkr-bench.c \
	$R/usr/lib/riscv64-linux-gnu/libvulkan.so.1 -Wl,--allow-shlib-undefined || exit 1
file $OUT/vkr-bench-rv64
# T2's t2-test for the guest too (when this tree has it), with lavapipe's outputs to compare against
S=$WT/gpu/webgpu/mesa/src/webgpu/tests/t2
if [ -f $S/t2-test.c ]; then
	LVP_ICD=$W/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
	rm -rf $OUT/t2 && mkdir -p $OUT/t2/spv
	for f in $S/*.vert $S/*.frag $S/*.comp $S/*.geom; do
		glslangValidator -V --target-env vulkan1.1 -o $OUT/t2/spv/$(basename $f).spv $f > /dev/null || exit 1
	done
	riscv64-linux-gnu-gcc -O1 -Wall -Wno-missing-braces -I$TREE/include -o $OUT/t2/t2-test $S/t2-test.c \
		$R/usr/lib/riscv64-linux-gnu/libvulkan.so.1 -Wl,--allow-shlib-undefined || exit 1
	cc -O1 -Wno-missing-braces -I$TREE/include -o $OUT/t2-host $S/t2-test.c -lvulkan || exit 1
	VK_DRIVER_FILES=$LVP_ICD $OUT/t2-host $OUT/t2/lvp $OUT/t2/spv > $OUT/t2/lvp.log 2>&1 || { tail $OUT/t2/lvp.log; exit 1; }
	tar -C $OUT -czf $OUT/t2.tgz t2 && rm -rf $OUT/t2 $OUT/t2-host
	ls -la $OUT/t2.tgz
fi
