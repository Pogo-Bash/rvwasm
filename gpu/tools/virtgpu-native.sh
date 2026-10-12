#!/bin/bash
# Native test of rv-virtgpu (rv/virtgpu.c), in the build VM: virgl_test_server (trixie chroot, llvmpipe) <- vtest <- rv's /dev/dri/renderD128 <- Mesa's virgl DRM winsys.
#   nt.sh 'PROGRAM ARGS' [SECONDS]      (in the VM; GUEST_ENV adds variables)
prog=$1; secs=${2:-120}
R=$HOME/rvwasm/debian-root-kde; RV=${RV:-$HOME/rvwasm/build-rv-virtgpu/rv}; X=$HOME/rvwasm/gpuguest/x
S=$HOME/rvwasm/webgpu/out/vt-virtgpu.sock
sudo pkill -f "virgl_test_server.*vt-virtgpu" 2>/dev/null; sudo rm -f $S
bash $HOME/rvwasm/webgpu/chroot-run.sh "virgl_test_server --use-egl-surfaceless --socket-path /out/vt-virtgpu.sock > /out/server-virtgpu.log 2>&1" &
for i in $(seq 100); do [ -S $S ] && break; sleep 0.1; done
sudo chmod 777 $S
set -- $prog; bin=$1; shift
[ -e "$X/usr/bin/$bin.riscv64-linux-gnu" ] && bin=$X/usr/bin/$bin.riscv64-linux-gnu
[ -e "$R/usr/bin/$bin" ] && bin=$R/usr/bin/$bin
timeout $secs /usr/bin/env -i PATH=$R/usr/bin RV_SYSROOT=$R HOME=/tmp XDG_CACHE_HOME=/tmp/virtgpu-cache RV_VIRTGPU=$S \
	$GUEST_ENV $RV $bin "$@"
echo "client rc $?"
sudo pkill -f "virgl_test_server.*vt-virtgpu"; sleep 0.3
echo "---- server log"; tail -5 $HOME/rvwasm/webgpu/out/server-virtgpu.log
