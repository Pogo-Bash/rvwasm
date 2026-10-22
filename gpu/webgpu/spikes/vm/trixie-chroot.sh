#!/bin/bash
# Spike S1/S2 host: a Debian trixie arm64 chroot (same Mesa 25.0.7 and Blender as the guest's Debian) in the VM.
set -e
C=$HOME/rvwasm/webgpu/trixie
if [ ! -x $C/usr/bin/apt-get ]; then
  sudo debootstrap --variant=minbase trixie $C http://deb.debian.org/debian
fi
sudo mount --bind /proc $C/proc 2>/dev/null || true
sudo mount --bind /dev $C/dev 2>/dev/null || true
sudo mount --bind /sys $C/sys 2>/dev/null || true
sudo chroot $C bash -c 'export DEBIAN_FRONTEND=noninteractive; apt-get update -q && apt-get install -y -q --no-install-recommends blender xvfb xauth mesa-utils mesa-vulkan-drivers libgl1-mesa-dri libegl1 libegl-mesa0 vulkan-tools spirv-tools glslang-tools 2>&1 | tail -5; dpkg -l blender libgl1-mesa-dri mesa-vulkan-drivers | tail -3'
