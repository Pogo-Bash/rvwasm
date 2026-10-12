#!/bin/bash
# vrend.{js,wasm} with this tree's gpu/vrend/rvgpu.c (rvg_res_attach / rvg_res_detach for imported resources),
# linked against the build VM's existing virglrenderer build (gpu/vrend/build.sh's, in $GPUHOST), which is only
# read. Output in ~/vdec/vrend-out.
#   gpu/vdec/build-vrend.sh        (in the build VM)
set -eu
T=$(cd "$(dirname "$0")/../.." && pwd)
EMSDK=${EMSDK:-$HOME/emsdk}
GPUHOST=${GPUHOST:-$HOME/gpuhost}
OUT=${OUT:-$HOME/vdec/vrend-out}
source $EMSDK/emsdk_env.sh > /dev/null 2>&1
VIRGL=$GPUHOST/virglrenderer
B=$VIRGL/build-wasm
PREFIX=$GPUHOST/prefix
mkdir -p $OUT
emcc -O2 -I$VIRGL/src -I$B -I$B/src -I$PREFIX/include -c $T/gpu/vrend/rvgpu.c -o $OUT/rvgpu.o
emcc -O2 $OUT/rvgpu.o $B/src/libvirglrenderer.a $PREFIX/lib/libepoxy.a -o $OUT/vrend.js --profiling-funcs \
	-sMODULARIZE=1 -sEXPORT_NAME=VrendModule -sENVIRONMENT=web,worker \
	-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1 -sALLOW_MEMORY_GROWTH=1 \
	-sSTACK_SIZE=4194304 -sSTACK_OVERFLOW_CHECK=1 \
	-sEXPORTED_RUNTIME_METHODS=GL,HEAPU8,HEAPU32 -sEXPORTED_FUNCTIONS=_malloc,_free \
	-sERROR_ON_UNDEFINED_SYMBOLS=1 -Wl,--error-limit=0
grep -q rvg_res_attach $OUT/vrend.js
ls -la $OUT/vrend.js $OUT/vrend.wasm
