#!/bin/bash
# Build Mesa 25.0.7 (zink only, no GL frontends) with vfront linked in, natively, in the build VM.
#   scripts/sync.sh 'bash ~/rvwasm/gpu-t6/vfront/scripts/mesa-build.sh [TARGETS...]'
set -e
M=$HOME/rvwasm/gpu-t6/mesa; B=$HOME/rvwasm/gpu-t6/build-native
[ -L $M/src/gallium/frontends/vfront ] || bash $HOME/rvwasm/gpu-t6/vfront/scripts/mesa-setup.sh
OPTS="-Dplatforms= -Dgallium-drivers=${DRIVERS:-zink,softpipe} -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled
 -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dshared-glapi=disabled -Dzstd=disabled
 -Dexpat=disabled -Dlibunwind=disabled -Dvalgrind=disabled -Dbuild-tests=false -Dshader-cache=disabled
 -Dvideo-codecs= -Dlmsensors=disabled -Dxmlconfig=disabled -Dbuildtype=debugoptimized -Db_ndebug=false"
cd $M
if [ ! -f $B/build.ninja ]; then
	meson setup $B $OPTS > $B.setup.log 2>&1 || { tail -40 $B.setup.log; exit 1; }
elif [ -n "$RECONFIGURE" ]; then
	meson configure $B $OPTS > $B.setup.log 2>&1 || { tail -40 $B.setup.log; exit 1; }
fi
T="$*"
[ -n "$T" ] || T="src/gallium/frontends/vfront/vfront-vtest src/gallium/frontends/vfront/vfront-replay"
ninja -C $B -j${JOBS:-6} $T 2>&1 | grep -v "^\[" | grep -v "^ninja: Entering" | grep -v "^cc \|^c++ " | head -${LINES_MAX:-80}
exit ${PIPESTATUS[0]}
