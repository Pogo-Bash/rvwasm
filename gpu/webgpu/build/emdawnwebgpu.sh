#!/bin/bash
# Build emdawnwebgpu (Dawn's webgpu.h for Emscripten) from the SAME Dawn checkout as Dawn native, so the
# page build and the native test bed compile against one webgpu.h. Output: $W/emdawnwebgpu/emdawnwebgpu_pkg
# (use with --use-port=$W/emdawnwebgpu/emdawnwebgpu_pkg/emdawnwebgpu.port.py). In the VM; incremental.
set -e
. "$(dirname "$0")/env.sh"
source ~/emsdk/emsdk_env.sh > /dev/null 2>&1
O=$W/emdawnwebgpu
if [ ! -f $O/build.ninja ]; then
	mkdir -p $O
	emcmake cmake -S $W/dawn -B $O -G Ninja -DCMAKE_BUILD_TYPE=Release -DDAWN_BUILD_SAMPLES=OFF \
		-DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DDAWN_SUPPORTS_CXX_MODULES=OFF \
		-DCMAKE_CXX_SCAN_FOR_MODULES=OFF > $O.cmake.log 2>&1 || { tail -30 $O.cmake.log; exit 1; }
fi
ninja -C $O -j${JOBS:-6} emdawnwebgpu_pkg 2>&1 | tail -3
ls $O/emdawnwebgpu_pkg
