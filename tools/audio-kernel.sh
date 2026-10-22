#!/bin/bash
# Build the kernel with the sound card, in the build VM, apart from the workspace's own kernel (which is what the
# dev server serves and is left alone): the workspace kernel's base commit with this tree's patches
# (kernel-rvwasm, -memsize, -lwfix, -audio) in ~/rvwasm/ksrc-audio, objects in ~/rvwasm/kbuild-audio, the result
# copied to ~/rvwasm/kernels/vmlinux-audio.wasm (and its System.map next to it).
#   tools/audio-kernel.sh [TREE]      TREE: this source tree as the VM has it (default: where this script is)
# The source tree is made again when the patches have changed since it was made. JOBS (8) for make.
set -e
S=$(realpath "${1:-$(dirname "$0")/..}")
LW=~/rvwasm/linux-wasm
K=~/rvwasm/ksrc-audio
B=~/rvwasm/kbuild-audio
. ~/rvwasm/env.sh
P="$S/patches/kernel-rvwasm.diff $S/patches/kernel-memsize.diff $S/patches/kernel-lwfix.diff $S/patches/kernel-audio.diff"
sum=$(cat $P | cksum | cut -d' ' -f1)
if [ ! -e $K/.patches-$sum ]; then
	echo "kernel source: $K (patches $sum)"
	# Other kernel trees (ksrc-mmu, ksrc-p1) are git worktrees of this one: removing it would take their object store.
	if [ -d $K/.git ] && [ "$(git -C $K worktree list 2> /dev/null | wc -l)" -gt 1 ]; then
		echo "audio-kernel: $K has worktrees (git -C $K worktree list); not removing it. Move them first." >&2
		exit 1
	fi
	rm -rf $K $B
	git clone -q --shared $LW/workspace/src/kernel $K
	git -C $K checkout -q 039e5f3e583f56f329657d1fe9945510dba10f41 2>/dev/null || true
	git -C $K apply $P
	touch $K/.patches-$sum
fi
M="make O=$B ARCH=wasm LLVM=$LW/tools/fake-llvm/ REAL_LLVM=$LW/workspace/install/llvm/bin/ CROSS_COMPILE=wasm32-unknown-unknown- HOSTCC=gcc"
mkdir -p $B
cd $K
[ -e $B/.config ] || $M wasm64_nommu_defconfig rvwasm.config > $B/config.log 2>&1
$M -j${JOBS:-8} > $B/build.log 2>&1 || { grep -E "error|Error" -A3 $B/build.log | head -60; exit 1; }
mkdir -p ~/rvwasm/kernels
cp $B/vmlinux ~/rvwasm/kernels/vmlinux-audio.wasm
cp $B/System.map ~/rvwasm/kernels/System.map-audio
grep -q "CONFIG_SND_WASM=y" $B/.config || { echo "the config has no CONFIG_SND_WASM"; exit 1; }
ls -la ~/rvwasm/kernels/vmlinux-audio.wasm
