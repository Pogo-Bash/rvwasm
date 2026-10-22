#!/bin/bash
# (In the VM) T5: the riscv64 guest's venus (Debian's mesa-vulkan-drivers, fetched into $W/out/t5/rvvk) under
# native rv (~/rvwasm/build-rv/rv, the interpreter) against vkr-vtest. Arguments: the program and its
# arguments, relative to the guest root (e.g. /tmp/vk/root/usr/bin/vulkaninfo --summary).
. "$(dirname "$0")/../../build/env.sh"
R=$HOME/rvwasm/debian-root-kde
RV=${RV:-$HOME/rvwasm/build-rv/rv}
O=$W/out/t5; V=$O/rvvk; mkdir -p $V
D=http://deb.debian.org/debian/pool/main
for p in m/mesa/mesa-vulkan-drivers_25.0.7-2+deb13u1_riscv64.deb v/vulkan-tools/vulkan-tools_1.4.304.0+dfsg1-1_riscv64.deb; do
	[ -f $V/$(basename $p) ] || curl -sf -o $V/$(basename $p) $D/$p || { echo "no $p"; exit 1; }
	[ -d $V/root/usr ] && [ $V/root -nt $V/$(basename $p) ] || dpkg -x $V/$(basename $p) $V/root
done
printf '{"file_format_version":"1.0.0","ICD":{"library_path":"%s/root/usr/lib/riscv64-linux-gnu/libvulkan_virtio.so","api_version":"1.3.305"}}\n' $V > $V/icd.json
LVP_ICD=$W/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
S=$O/rv.sock; rm -f $S
WGVK_DAWN_DRIVER_FILES=$LVP_ICD VKR_VTEST_EXIT_IDLE=${IDLE:-10} RVGPU_VKR_STATS=1 timeout ${TMO:-600} $B/native/src/webgpu/vkr/vkr-vtest --socket $S > $O/rv-server.log 2>&1 &
for i in $(seq 100); do [ -S $S ] && break; sleep 0.1; done
t0=$(date +%s.%N)
/usr/bin/timeout ${CTMO:-500} /usr/bin/env -i PATH=$R/usr/bin RV_SYSROOT=$R HOME=/tmp VN_DEBUG=vtest \
	VTEST_SOCKET_NAME=$S VK_DRIVER_FILES=$V/icd.json $RV "$@" 2>&1 | grep -v -E "^$" | tail -${TAIL:-25}
echo "client rc ${PIPESTATUS[0]} in $(echo "$(date +%s.%N) - $t0" | bc) s"
wait
rm -f $S
tail -5 $O/rv-server.log
