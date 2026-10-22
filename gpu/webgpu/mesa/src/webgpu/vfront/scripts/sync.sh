#!/bin/bash
# From the Mac: send gpu/webgpu/vfront (with these scripts) to the build VM, which mounts only the main
# checkout, not the worktrees; then optionally run a command there.
#   gpu/webgpu/vfront/scripts/sync.sh 'bash ~/rvwasm/gpu-t6/vfront/scripts/mesa-build.sh'
# VM layout (~/rvwasm/gpu-t6): vfront/ (this directory's parent), mesa/ (Mesa 25.0.7 + mesa-vfront.diff,
# vfront linked in at src/gallium/frontends/vfront), build-native/ (meson build). In the trixie chroot
# (scripts/t6-chroot.sh) the same directory is /t6.
set -eu
export PATH=~/dev/rvwasm/tools/lima/bin:$PATH
SRC=$(cd "$(dirname "$0")/.." && pwd)
COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC/.." -cf - vfront | \
	limactl shell --workdir / rvwasm bash -c "mkdir -p ~/rvwasm/gpu-t6 && rm -rf ~/rvwasm/gpu-t6/vfront.new && mkdir ~/rvwasm/gpu-t6/vfront.new && tar -C ~/rvwasm/gpu-t6/vfront.new -xf - && rsync -a --delete --checksum ~/rvwasm/gpu-t6/vfront.new/vfront/ ~/rvwasm/gpu-t6/vfront/ && rm -rf ~/rvwasm/gpu-t6/vfront.new"
[ $# -gt 0 ] && limactl shell --workdir / rvwasm bash -c "$1"
exit 0
