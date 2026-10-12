#!/bin/bash
# Run a command on the machine under Node (tools/node-machine.js) with this tree's gpuserver, wlserver,
# desktop script and debian-enter layered over an initramfs that has rv and hostnode (build/nm/, from a
# vm-stage of this or another tree), and the stub executor (gpu/tools/gpu-stub.js) standing in for the
# page. Riscv64 programs to try (mesa-utils-bin from Debian, not on the disks) go in build/gpu-bins/ and
# are /run/rvgpu/bin/ in the guest; their data (glmark2's) in build/gpu-share/, /run/rvgpu/share/ there.
#   [DISK=wl] [HOOK=gpu/tools/gpu-stub.js] [V=1] gpu/tools/nm-run.sh 'COMMAND' [TIMEOUT_MS]
set -e
cd "$(dirname "$0")/../.."
N=build/nm
rm -rf $N/stage
mkdir -p $N/stage/usr/lib/wlserver $N/stage/usr/lib/rvgpu/bin $N/stage/bin
cp -r wlserver/. $N/stage/usr/lib/wlserver/
cp tools/x/desktop $N/stage/usr/lib/wlserver/desktop
cp gpu/server/*.js $N/stage/usr/lib/rvgpu/
[ -d build/gpu-bins ] && cp build/gpu-bins/* $N/stage/usr/lib/rvgpu/bin/
[ -d build/gpu-share ] && cp -r build/gpu-share $N/stage/usr/lib/rvgpu/share
cp tools/debian-enter $N/stage/bin/debian
chmod 755 $N/stage/bin/debian $N/stage/usr/lib/wlserver/desktop $N/stage/usr/lib/rvgpu/bin/* 2> /dev/null || true
(cd $N/stage && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2>/dev/null | gzip -1 > ../extra.cpio.gz)
opts=(--disk "${DISK:-wl}" --hook "${HOOK:-gpu/tools/gpu-stub.js}")
[ -n "$V" ] && opts+=(--verbose)
exec node --experimental-wasm-memory64 tools/node-machine.js --runtime $N/rt --kernel $N/vmlinux.wasm \
	--initramfs $N/initramfs.cpio.gz --initramfs $N/extra.cpio.gz --timeout "${2:-120000}" "${opts[@]}" "$1"
