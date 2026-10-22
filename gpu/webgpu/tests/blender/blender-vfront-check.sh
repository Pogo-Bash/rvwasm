#!/bin/bash
# (VM) VK-12: Blender 4.3.2 Workbench through vfront-vtest on wgvk (virpipe) against the lavapipe render (zink on
# the chroot's lavapipe, blender-run.sh via chroot-wgvk.sh lvp). Passes when at most 0.5% of the pixels differ by more than 8/255.
#   VFRONT_BUILD=vk-03 gpu/webgpu/build/vm.sh gpu/webgpu/tests/blender/blender-vfront-check.sh     (REF=png: use that reference)
S=$(cd "$(dirname "$0")" && pwd)
G=$HOME/rvwasm/gpu-t6/blender
T=$S/../../mesa/src/webgpu/vfront/scripts/t6-chroot.sh
mkdir -p $G; cp $S/imgdiff.py $G/
ref=${REF:-$G/ref-lvp-workbench.png}
if [ ! -s "$ref" ]; then
	O=$HOME/rvwasm/webgpu/out/t2; mkdir -p $O
	cp $S/blender-run.sh $S/imgdiff.py $S/passes.py $O/
	bash $S/chroot-wgvk.sh lvp 'bash /out/t2/blender-run.sh workbench ref' || exit 1
	cp $O/ref-workbench/render_0001.png $ref
fi
bash $S/blender-vfront.sh workbench check || exit 1
out=$(bash $T "blender -b --factory-startup --python /t6/blender/imgdiff.py -- /t6/blender/$(basename $ref) /t6/blender/check-workbench/render_0001.png 8" 2>&1 | grep imgdiff)
echo "$out"
pct=$(echo "$out" | sed -n 's/.*(\([0-9.]*\)%).*/\1/p')
awk -v p="${pct:-100}" 'BEGIN { if (p <= 0.5) { print "PASS: " p "% <= 0.5%"; exit 0 } print "FAIL: " p "% > 0.5%"; exit 1 }'
