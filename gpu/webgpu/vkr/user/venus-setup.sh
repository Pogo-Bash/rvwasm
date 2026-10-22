#!/bin/bash
# (In the machine, inside `debian`) T5: Vulkan through venus to the page's vkr, without touching the disk:
# Debian's riscv64 mesa-vulkan-drivers and vulkan-tools unpacked into /tmp/vk (an in-memory /tmp), the test
# programs from the dev server, and the environment in /tmp/vk/env.
#   bash venus-setup.sh            fetch and unpack (once per boot)
#   . /tmp/vk/env                  VN_DEBUG, VK_DRIVER_FILES, PATH for the venus driver
#   bash /tmp/vk/gpuserver.sh      start gpuserver alone (no window server) for programs without windows
U=${U:-http://10.0.5.1:8000}
D=http://deb.debian.org/debian/pool/main
mkdir -p /tmp/vk && cd /tmp/vk || exit 1
for p in m/mesa/mesa-vulkan-drivers_25.0.7-2+deb13u1_riscv64.deb v/vulkan-tools/vulkan-tools_1.4.304.0+dfsg1-1_riscv64.deb; do
	[ -f $(basename $p) ] || curl -sf -O $D/$p || { echo "venus-setup: could not fetch $D/$p"; exit 1; }
done
for d in *.deb; do dpkg -x $d root; done
for f in vkr-bench-rv64 add.spv t2.tgz; do curl -sf -o $f $U/t5/$f || echo "venus-setup: no $U/t5/$f"; done
chmod +x vkr-bench-rv64; [ -f t2.tgz ] && tar -xzf t2.tgz
printf '{"file_format_version":"1.0.0","ICD":{"library_path":"/tmp/vk/root/usr/lib/riscv64-linux-gnu/libvulkan_virtio.so","api_version":"1.3.305"}}\n' > icd.json
cat > env <<'ENV'
export VN_DEBUG=vtest VK_DRIVER_FILES=/tmp/vk/icd.json PATH=/tmp/vk/root/usr/bin:$PATH
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xdg}
export VTEST_SOCKET_NAME=$XDG_RUNTIME_DIR/rvgpu-0
ENV
cat > gpuserver.sh <<'SRV'
. /tmp/vk/env; mkdir -p $XDG_RUNTIME_DIR
[ -S $XDG_RUNTIME_DIR/rvgpu-0 ] || { HOSTNODE_HOME=/run/rvgpu RVGPU_STATS=1 /run/hostnode/hostnode > /tmp/rvgpu.log 2>&1 & }
for i in $(seq 300); do [ -f $XDG_RUNTIME_DIR/rvgpu-0.ready ] && break; sleep 0.1; done
[ -f $XDG_RUNTIME_DIR/rvgpu-0.ready ] && echo "gpuserver ready" || { echo "no executor (see /tmp/rvgpu.log)"; tail -5 /tmp/rvgpu.log; }
SRV
echo "venus-setup: $(ls root/usr/lib/riscv64-linux-gnu/libvulkan_virtio.so root/usr/bin/vulkaninfo root/usr/bin/vkcube vkr-bench-rv64 2>&1 | wc -l) of 4 files there"
