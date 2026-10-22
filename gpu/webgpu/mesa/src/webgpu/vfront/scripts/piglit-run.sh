#!/bin/bash
# In the chroot: run a piglit selection through vfront-vtest (SERVER=vfront, default) or virgl_test_server on
# llvmpipe (SERVER=vrend), with the guest-side stock Mesa virgl driver (virpipe).
#   piglit-run.sh NAME PROFILE 'REGEX' [JOBS]     results in /t6/piglit/NAME, summary on stdout
name=$1; profile=$2; regex=$3; jobs=${4:-4}
export USER=t6 LOGNAME=t6
SOCK=/t6/run/pg-$name.sock
[ "$SERVER" = external ] || rm -f $SOCK
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
if [ "$SERVER" = external ]; then
	SOCK=$VTEST_SOCKET_NAME
elif [ "$SERVER" = vrend ]; then
	virgl_test_server --use-egl-surfaceless --multi-clients --socket-path $SOCK > /t6/log/pg-$name-server.log 2>&1 &
else
	LIBGL_ALWAYS_SOFTWARE=1 /t6/build-native/src/gallium/frontends/vfront/vfront-vtest --socket $SOCK > /t6/log/pg-$name-server.log 2>&1 &
fi
spid=$!
[ "$SERVER" = external ] && spid=
for i in $(seq 100); do [ -S $SOCK ] && break; sleep 0.1; done
# (no wflinfo in the chroot: without this piglit "fast-skips" every shader_runner test as GL 0.0)
export GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1 VTEST_SOCKET_NAME=$SOCK PIGLIT_PLATFORM=surfaceless_egl PIGLIT_NO_FAST_SKIP=1
mkdir -p /t6/piglit
# (TESTLIST=FILE: exactly the tests named in it, one per line, instead of the regex)
if [ -n "$TESTLIST" ]; then sel=(--test-list "$TESTLIST"); else sel=(-t "$regex"); fi
timeout ${PIGLIT_TIMEOUT_S:-1800} piglit run -p surfaceless_egl -j $jobs --timeout 60 -o -l dummy "${sel[@]}" $profile /t6/piglit/$name > /t6/log/pg-$name-run.log 2>&1
echo "piglit rc $?"
kill $spid 2>/dev/null; wait $spid 2>/dev/null
piglit summary console -s /t6/piglit/$name 2>/dev/null | tail -12
