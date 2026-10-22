#!/bin/bash
# In the chroot: one piglit binary against a fresh server (SERVER=vfront default, or vrend), and whether the
# server survived it.   pg-one.sh 'CMD ARGS'     (GDB=1 runs vfront-vtest under gdb and prints the backtrace)
export USER=t6 LOGNAME=t6
SOCK=/t6/run/one-$$.sock
BIN=${VFRONT_VTEST:-/t6/build-native/src/gallium/frontends/vfront/vfront-vtest}
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
if [ "$SERVER" = external ]; then
	SOCK=$VTEST_SOCKET_NAME
elif [ "$SERVER" = vrend ]; then
	virgl_test_server --use-egl-surfaceless --socket-path $SOCK > /t6/log/one-server.log 2>&1 &
elif [ -n "$GDB" ]; then
	LIBGL_ALWAYS_SOFTWARE=1 gdb -q -batch -ex run -ex bt -ex 'info registers pc' --args $BIN --socket $SOCK > /t6/log/one-server.log 2>&1 &
else
	LIBGL_ALWAYS_SOFTWARE=1 $BIN --socket $SOCK > /t6/log/one-server.log 2>&1 &
fi
spid=$!
[ "$SERVER" = external ] && spid=
for i in $(seq 100); do [ -S $SOCK ] && break; sleep 0.1; done
GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1 VTEST_SOCKET_NAME=$SOCK PIGLIT_PLATFORM=surfaceless_egl \
	timeout ${T:-120} $1 2>&1 | tail -${LINES_OUT:-15}
echo "test rc ${PIPESTATUS[0]}"
sleep 0.5
if [ -z "$spid" ]; then :; elif kill -0 $spid 2>/dev/null; then echo "server alive"; kill $spid; else echo "SERVER DIED"; fi
wait $spid 2>/dev/null
tail -${SLINES:-40} /t6/log/one-server.log
[ "$SERVER" = external ] || rm -f $SOCK
