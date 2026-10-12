#!/bin/bash
# Native reference run in the build VM: virgl_test_server (Ubuntu arm64, host Mesa llvmpipe, EGL surfaceless)
# and a riscv64 Mesa client under rv (interpreter) talking vtest over a Unix socket.
#   gpu/native-vtest.sh 'PROGRAM ARGS' [SERVER_ARGS] [TIMEOUT_S]
# Runs in the VM: limactl shell --workdir / rvwasm bash ~/wt/gpu-guest/gpu/native-vtest.sh ...
R=$HOME/rvwasm/debian-root-kde
RV=$HOME/rvwasm/build-rv-gpuguest/rv
X=$HOME/rvwasm/gpuguest/x
SOCK=/tmp/gpuguest-vt.sock
prog=$1; sargs=${2:---use-egl-surfaceless}; to=${3:-60}
rm -f $SOCK
virgl_test_server $sargs --socket-path $SOCK > /tmp/gpuguest-server.log 2>&1 &
spid=$!
for i in $(seq 50); do [ -S $SOCK ] && break; sleep 0.1; done
set -- $prog
bin=$1; shift
[ -e "$X/usr/bin/$bin.riscv64-linux-gnu" ] && bin=$X/usr/bin/$bin.riscv64-linux-gnu
[ -e "$X/usr/bin/$bin" ] && bin=$X/usr/bin/$bin
[ -e "$R/usr/bin/$bin" ] && bin=$R/usr/bin/$bin
timeout $to /usr/bin/env -i PATH=$R/usr/bin RV_SYSROOT=$R HOME=/root \
	GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1 VTEST_SOCKET_NAME=$SOCK EGL_PLATFORM=surfaceless \
	VK_ICD_FILENAMES=$HOME/rvwasm/gpuguest/virtio_icd.json VN_DEBUG=${VN_DEBUG:-vtest} \
	$GUEST_ENV $RV $bin "$@"
echo "client rc $?"
kill $spid 2>/dev/null; wait $spid 2>/dev/null
echo "---- server log"; tail -20 /tmp/gpuguest-server.log
