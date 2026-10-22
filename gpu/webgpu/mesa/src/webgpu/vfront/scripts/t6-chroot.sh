#!/bin/bash
# Run a command in the trixie chroot (Mesa 25.0.7 + lavapipe + virgl_test_server + piglit) with this track's
# VM directory at /t6:  t6-chroot.sh 'CMD'
C=$HOME/rvwasm/webgpu/trixie
mountpoint -q $C/proc || sudo mount --bind /proc $C/proc
mountpoint -q $C/dev || sudo mount --bind /dev $C/dev
mountpoint -q $C/sys || sudo mount --bind /sys $C/sys
mountpoint -q $C/dev/shm || sudo mount --bind /dev/shm $C/dev/shm
sudo mkdir -p $C/t6 $C/tmp
mountpoint -q $C/t6 || sudo mount --bind $HOME/rvwasm/gpu-t6 $C/t6
sudo chroot --userspec=$(id -u):$(id -g) $C /usr/bin/env -i PATH=/usr/bin:/bin HOME=/t6 XDG_RUNTIME_DIR=/t6/run bash -c "mkdir -p /t6/run; $1"
