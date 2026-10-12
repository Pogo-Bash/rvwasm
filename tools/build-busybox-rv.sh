#!/bin/bash
# Build a riscv64 BusyBox (glibc) from the same source the Linux/Wasm system
# uses, without the wasm patch. Run inside the build VM.
#
#   build-busybox-rv.sh           static:  ~/rvwasm/busybox-rv/busybox
#   build-busybox-rv.sh dynamic   dynamic: ~/rvwasm/busybox-rv-dyn/busybox
#                                 (needs a sysroot to run: /usr/riscv64-linux-gnu)

set -e

W="${RVWASM_WORK:-$HOME/rvwasm}"
SRC="$W/linux-wasm/workspace/src/busybox"
MAKE=(make -s ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu-)
DIR="$W/busybox-rv"
[ "$1" = dynamic ] && DIR="$W/busybox-rv-dyn"

if [ ! -d "$DIR" ]; then
	# The wasm patch is the single commit on top of the release tag.
	git -C "$SRC" worktree add -q --detach "$DIR" HEAD~1
fi
cd "$DIR"

"${MAKE[@]}" defconfig > /dev/null
# Without `tc`, which no longer builds against current kernel headers.
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
if [ "$1" != dynamic ]; then
	sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
fi
yes "" | "${MAKE[@]}" oldconfig > /dev/null
"${MAKE[@]}" -j"$(nproc)" > build.log 2>&1
ls -la busybox
