#!/bin/bash
# Run a command inside the trixie chroot (mounts /proc /dev /sys /tmp once).  chroot-run.sh 'CMD'
C=$HOME/rvwasm/webgpu/trixie
mountpoint -q $C/proc || sudo mount --bind /proc $C/proc
mountpoint -q $C/dev || sudo mount --bind /dev $C/dev
mountpoint -q $C/sys || sudo mount --bind /sys $C/sys
mkdir -p $HOME/rvwasm/webgpu/out; sudo mkdir -p $C/out; mountpoint -q $C/out || sudo mount --bind $HOME/rvwasm/webgpu/out $C/out
sudo chroot $C /usr/bin/env -i PATH=/usr/bin:/bin HOME=/root bash -c "$1"
