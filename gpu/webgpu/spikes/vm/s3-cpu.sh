#!/bin/bash
# Spike S3: guest CPU per frame for the two guest layerings, same program, same host (trixie chroot: virgl_test_server
# 1.1.0 with llvmpipe for virgl, lavapipe for venus). Guest = Debian riscv64 Mesa 25.0.7 under native rv (interpreter).
#   s3-cpu.sh virgl|zinkvenus 'PROGRAM ARGS' SECONDS
mode=$1; prog=$2; secs=${3:-60}
R=$HOME/rvwasm/debian-root-kde; RV=$HOME/rvwasm/build-rv-webgpu/rv; X=$HOME/rvwasm/gpuguest/x
S=$HOME/rvwasm/webgpu/out/vt-$mode.sock
sudo rm -f $S
args="--use-egl-surfaceless --no-fork"; [ $mode = zinkvenus ] && args="--venus --use-egl-surfaceless --no-fork"
bash $HOME/rvwasm/webgpu/chroot-run.sh "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json virgl_test_server $args --socket-path /out/vt-$mode.sock > /out/server-$mode.log 2>&1" &
for i in $(seq 100); do [ -S $S ] && break; sleep 0.1; done
sudo chmod 777 $S
set -- $prog; bin=$1; shift
[ -e "$X/usr/bin/$bin.riscv64-linux-gnu" ] && bin=$X/usr/bin/$bin.riscv64-linux-gnu
[ -e "$R/usr/bin/$bin" ] && bin=$R/usr/bin/$bin
if [ $mode = virgl ]; then E="GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1"
else E="MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1 VK_ICD_FILENAMES=$HOME/rvwasm/gpuguest/virtio_icd.json VN_DEBUG=vtest"; fi
timeout $secs /usr/bin/env -i PATH=$R/usr/bin RV_SYSROOT=$R HOME=/tmp XDG_CACHE_HOME=/tmp/webgpu-cache-$mode $E VTEST_SOCKET_NAME=$S \
	EGL_PLATFORM=surfaceless $GUEST_ENV $RV $bin "$@"
echo "client rc $?"
sudo pkill -f "virgl_test_server.*vt-$mode" ; sleep 0.5
echo "---- server log"; tail -5 $HOME/rvwasm/webgpu/out/server-$mode.log
