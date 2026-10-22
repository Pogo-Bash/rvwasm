#!/bin/bash
# (In the VM) Dawn native for wgvk: a pkg-config file (so Mesa's meson finds webgpu.h and libwebgpu_dawn) and
# a run directory $W/dawn-run that wgvk links against (rpath). Dawn is built by spikes/vm/dawn-native.sh at
# $W/dawn-native from $W/dawn (the commit in gpu/webgpu/build/dawn.pin).
#
# Why the run directory: as an ICD, wgvk is called by the Vulkan loader with the loader's lock held, and
# wgvk then makes a Dawn device, whose Vulkan backend calls vkCreateInstance: on the same loader that
# deadlocks. Dawn opens libvulkan.so.1 from its own directory first, so $W/dawn-run holds a hard link to
# libwebgpu_dawn.so and a COPY of the system loader (a copy, not a link: a different file is loaded as a
# second, independent loader, with its own lock). That loader finds wgvk again; wgvk reports no device
# while it is making its Dawn device, so Dawn ends up on lavapipe.
set -e
. "$(dirname "$0")/env.sh"
mkdir -p $W/pc $W/dawn-run
# (a hard link: meson resolves symbolic links when it makes the rpath)
ln -f $W/dawn-native/src/dawn/native/libwebgpu_dawn.so $W/dawn-run/libwebgpu_dawn.so
L=$(readlink -f /lib/aarch64-linux-gnu/libvulkan.so.1)
cmp -s $L $W/dawn-run/libvulkan.so.1 || cp $L $W/dawn-run/libvulkan.so.1
cat > $W/pc/webgpu_dawn.pc.new <<PC
Name: webgpu_dawn
Description: Dawn native (webgpu.h), monolithic shared library, from the wgvk run directory
Version: 0
Cflags: -I$W/dawn/include -I$W/dawn-native/gen/include
Libs: -L$W/dawn-run -Wl,-rpath,$W/dawn-run -lwebgpu_dawn
PC
cmp -s $W/pc/webgpu_dawn.pc.new $W/pc/webgpu_dawn.pc && rm $W/pc/webgpu_dawn.pc.new || mv $W/pc/webgpu_dawn.pc.new $W/pc/webgpu_dawn.pc
