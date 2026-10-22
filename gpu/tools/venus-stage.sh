#!/bin/sh
# Vulkan in the machine without a setup script (VK-08a): the staged venus driver. Puts the riscv64 programs and drivers
# the machine's first file system carries as /usr/lib/rvgpu/venus (Debian sees it as /run/rvgpu/venus, tools/debian-enter)
# into DEST and writes the ICD files that name them. The disk has the Vulkan loader (libvulkan1) but not the drivers
# (mesa-vulkan-drivers, vulkan-tools: VK-08b adds them to the disk; tools/x/desktop prefers the disk's ICD files once
# they are there, so these are the ones to drop then).
#
#   gpu/tools/venus-stage.sh DEST FILE...
#
# FILE: libvulkan_virtio.so (venus, the page's executor: Mesa 25.0.7 as Debian trixie builds it), libvulkan_lvp.so
# (lavapipe, what vulkaninfo falls to where the executor has no Vulkan), vulkaninfo, vkcube, vkr-bench-rv64 and
# add.spv (T5's micro-benchmark and its shader). Those given are copied; the ICD files are written for the drivers given.
# Callers: tools/stage.sh (the page's initramfs), tools/gpu-nm.sh (node-machine with the real executor).
set -e
D=$1; shift
[ -n "$D" ] && [ $# -gt 0 ] || { sed -n '2,/^set -e/p' "$0" | sed '$d' | head -16; exit 2; }
mkdir -p "$D"
for f in "$@"; do
	cp "$f" "$D/"
done
chmod 755 "$D"/* 2> /dev/null || true
for pair in virtio:libvulkan_virtio.so lvp:libvulkan_lvp.so; do
	n=${pair%%:*}
	so=${pair#*:}
	[ -e "$D/$so" ] || continue
	printf '{"file_format_version":"1.0.0","ICD":{"library_path":"/run/rvgpu/venus/%s","api_version":"1.3.305"}}\n' "$so" > "$D/${n}_icd.json"
done
