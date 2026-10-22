#!/bin/bash
# In the VM: build tools/vrend_replay.c inside the trixie chroot against Debian's libvirglrenderer (1.1.0),
# with virglrenderer's public header from the 1.1.1 checkout gpu-host uses. Output: ~/rvwasm/gpu-t6/vrend-replay
set -e
T=$HOME/rvwasm/gpu-t6
mkdir -p $T/inc
cp $HOME/gpuhost/virglrenderer/src/virglrenderer.h $T/inc/
cp $T/mesa/src/virtio/virtio-gpu/virgl_hw.h $T/inc/
printf "#define VIRGL_MAJOR_VERSION 1\n#define VIRGL_MINOR_VERSION 1\n#define VIRGL_MICRO_VERSION 0\n" > $T/inc/virgl-version.h
bash $T/vfront/scripts/t6-chroot.sh "gcc -O2 -g -Wall -I/t6/inc -I/t6/vfront/tools -o /t6/vrend-replay /t6/vfront/tools/vrend_replay.c /usr/lib/aarch64-linux-gnu/libvirglrenderer.so.1 && echo built vrend-replay"
