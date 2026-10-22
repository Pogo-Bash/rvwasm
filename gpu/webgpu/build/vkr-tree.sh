#!/bin/bash
# (In the VM) T5: this build's virglrenderer tree for vkr (gpu/webgpu/INTERFACES.md 7): a git worktree of
# $W/virglrenderer at the pin in gpu/webgpu/vkr/virglrenderer.pin with gpu/webgpu/vkr/virglrenderer-page.diff
# applied (afresh whenever the patch changes, like tree.sh), the venus protocol's renderer headers and vkr's
# generated headers in its gen/, linked into this build's Mesa tree as src/vkr-src (src/webgpu/vkr/meson.build
# builds vkr from there when the link exists). Edit files in the tree, then gpu/webgpu/vkr/mkdiff.sh.
set -e
. "$(dirname "$0")/env.sh"
VPIN=$(grep -v '^#' $WT/gpu/webgpu/vkr/virglrenderer.pin | sed -n 1p | cut -d' ' -f1)
VURL=$(grep -v '^#' $WT/gpu/webgpu/vkr/virglrenderer.pin | sed -n 1p | cut -d' ' -f2)
PPIN=$(grep -v '^#' $WT/gpu/webgpu/vkr/virglrenderer.pin | sed -n 2p | cut -d' ' -f1)
PURL=$(grep -v '^#' $WT/gpu/webgpu/vkr/virglrenderer.pin | sed -n 2p | cut -d' ' -f2)
V=$W/virglrenderer
P=$W/venus-protocol
[ -d $V ] || git clone -q --depth 50 $VURL $V
git -C $V cat-file -e $VPIN^{commit} 2>/dev/null || git -C $V fetch -q --depth 1 origin $VPIN
[ -d $P ] || git clone -q --depth 1 $PURL $P
git -C $P cat-file -e $PPIN^{commit} 2>/dev/null || { git -C $P fetch -q --depth 1 origin $PPIN; git -C $P checkout -q $PPIN; }
VT=$W/vkr-trees/$NAME
if [ ! -d $VT ]; then
	mkdir -p $W/vkr-trees
	git -C $V worktree add -q --detach $VT $VPIN
fi
D=$WT/gpu/webgpu/vkr/virglrenderer-page.diff
h="$VPIN $PPIN $(sha1sum < $D | cut -c1-40)"
if [ "$(cat $VT/.rvgpu-stamp 2>/dev/null)" != "$h" ]; then
	git -C $VT checkout -q --detach $VPIN
	git -C $VT reset -q --hard
	git -C $VT clean -qfdx
	[ ! -s $D ] || git -C $VT apply --whitespace=nowarn $D
	mkdir -p $VT/gen
	(cd $P && git rev-parse --short=8 HEAD > $VT/gen/banner.in && sed "s/@VCS_TAG@/$(cat $VT/gen/banner.in)/" templates/banner.in > $VT/gen/banner 2>/dev/null || true)
	python3 $P/vn_protocol.py --srcdir $P --outdir $VT/gen --banner $VT/gen/banner --renderer > /dev/null
	python3 $VT/src/venus/vkr_device_object.py -o $VT/gen $VT/src/venus/vkr_device_object.json
	cp -r $P/include/vulkan $P/include/vk_video $VT/gen/
	echo "$h" > $VT/.rvgpu-stamp
	echo "vkr-tree.sh: $VT at ${VPIN:0:12} + virglrenderer-page.diff"
fi
ln -sfn $VT $TREE/src/vkr-src
