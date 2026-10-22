#!/bin/bash
# (In the VM) make or refresh this build's Mesa tree: a git worktree of $W/mesa-main at the pinned commit,
# with patches/mesa-page.diff applied and src/webgpu linked to this worktree's gpu/webgpu/mesa/src/webgpu.
# The patch is applied afresh whenever it changed (a stamp holds its hash); the tree's own edits are lost
# then, so make Mesa changes in patches/mesa-page.diff (gpu/webgpu/build/mkpatch.sh writes it from a tree).
set -e
. "$(dirname "$0")/env.sh"
M=$W/mesa-main
if ! git -C $M cat-file -e $PIN^{commit} 2>/dev/null; then
	git -C $M fetch -q --depth 1 origin $PIN
fi
if [ ! -d $TREE ]; then
	mkdir -p $W/trees
	git -C $M worktree add -q --detach $TREE $PIN
fi
P=$WT/patches/mesa-page.diff
h="$PIN $(sha1sum < $P | cut -c1-40)"
if [ "$(cat $TREE/.rvgpu-stamp 2>/dev/null)" != "$h" ]; then
	git -C $TREE checkout -q --detach $PIN
	git -C $TREE reset -q --hard
	git -C $TREE clean -qfd -e src/webgpu
	[ ! -s $P ] || git -C $TREE apply --whitespace=nowarn $P
	echo "$h" > $TREE/.rvgpu-stamp
	echo "tree.sh: $TREE at ${PIN:0:12} + mesa-page.diff"
fi
ln -sfn $WT/gpu/webgpu/mesa/src/webgpu $TREE/src/webgpu
