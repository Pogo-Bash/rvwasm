#!/bin/bash
# (In the VM) T5 native test: vkr-vtest (gpuserver + the page executor in one process: vkr on wgvk on Dawn on
# lavapipe) and a venus client in the trixie arm64 chroot (Mesa 25.0.7 libvulkan_virtio, VN_DEBUG=vtest).
# nt.sh 'CLIENT COMMAND'   (runs inside the chroot with the venus env)
. "$(dirname "$0")/../../build/env.sh"
LVP_ICD=$W/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
O=$W/out/t5; mkdir -p $O; rm -f $O/vt.sock
WGVK_DAWN_DRIVER_FILES=$LVP_ICD VKR_VTEST_STATS=${STATS:-0} VKR_VTEST_EXIT_IDLE=${IDLE:-3} timeout ${TMO:-120} \
	$B/native/src/webgpu/vkr/vkr-vtest --socket $O/vt.sock > $O/server.log 2>&1 &
SP=$!
for i in $(seq 100); do [ -S $O/vt.sock ] && break; sleep 0.1; done
bash $W/chroot-run.sh "cd /out/t5; export VN_DEBUG=${VN_DEBUG:-vtest} VTEST_SOCKET_NAME=/out/t5/vt.sock VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json $CENV; timeout ${CTMO:-90} $1" 2>&1 | tail -${TAIL:-60}
echo "client rc ${PIPESTATUS[0]}"
wait $SP; echo "server rc $?"
echo ==== server log; tail -${STAIL:-30} $O/server.log
