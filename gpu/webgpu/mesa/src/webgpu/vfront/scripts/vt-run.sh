#!/bin/bash
# In the chroot: start vfront-vtest (or virgl_test_server with SERVER=vrend), run a client through virpipe,
# stop the server.   vt-run.sh 'CLIENT ARGS' [CAPTURE_NAME] [TIMEOUT_S]
# Captures go to /t6/cap/NAME.vfcap. Extra client environment in $CENV, server environment in $SENV.
client=$1; cap=$2; to=${3:-60}
SOCK=/t6/run/vt-$$.sock
mkdir -p /t6/cap /t6/log
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
# zink takes a CPU device (lavapipe) only when told software is fine
SENV="LIBGL_ALWAYS_SOFTWARE=1 $SENV"
if [ "$SERVER" = vrend ]; then
	env $SENV virgl_test_server --use-egl-surfaceless --socket-path $SOCK > /t6/log/server.log 2>&1 &
else
	capargs=""
	[ -n "$cap" ] && capargs="--capture /t6/cap/$cap.vfcap"
	env $SENV /t6/build-native/src/gallium/frontends/vfront/vfront-vtest -v --once --socket $SOCK $capargs > /t6/log/server.log 2>&1 &
fi
spid=$!
for i in $(seq 100); do [ -S $SOCK ] && break; sleep 0.1; done
[ -S $SOCK ] || { echo "server did not start"; cat /t6/log/server.log; exit 1; }
env GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1 VTEST_SOCKET_NAME=$SOCK EGL_PLATFORM=surfaceless $CENV timeout $to $client
echo "client rc $?"
sleep 0.3
kill $spid 2>/dev/null; wait $spid 2>/dev/null
echo "---- server log"; tail -${SLOG:-25} /t6/log/server.log
rm -f $SOCK
