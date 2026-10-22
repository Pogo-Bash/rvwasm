#!/bin/bash
# Spike S4: link spirv2nir natively and to wasm (node), then run both over Blender's SPIR-V (zink, P4 profile).
set -e
S=$HOME/rvwasm/webgpu/mesa-wasm-spike; M=$HOME/rvwasm/webgpu/mesa-25.0.7; N=$S/native
source ~/emsdk/emsdk_env.sh > /dev/null 2>&1
INC="-I$M/include -I$M/src -I$M/src/compiler -I$M/src/compiler/nir -I$M/src/compiler/spirv -I$M/src/util -I$N/src -I$N/src/compiler -I$N/src/compiler/nir -I$N/src/compiler/spirv -I$N/src/util"
cd $S
gcc -O2 $INC -DHAVE_PTHREAD -DHAVE_ENDIAN_H -DHAVE_STRUCT_TIMESPEC -D_GNU_SOURCE -DHAVE_SECURE_GETENV -DNDEBUG -DMESA_DEBUG=0 -o spirv2nir-native spirv2nir.c $N/src/compiler/spirv/libvtn.a $N/src/compiler/nir/libnir.a $N/src/compiler/libcompiler.a $N/src/util/libmesa_util.a $N/src/c11/impl/libmesa_util_c11.a $(ls $N/src/util/blake3/*.a 2>/dev/null) -lm -lpthread -lz -ldl $(ls $N/src/util/libmesa_util_sse41.a 2>/dev/null) 2>&1 | grep -v warning | head -20 || true
ls -la spirv2nir-native
O=$(ls wasm-objs/*.o | grep -v os_file_notify)
emcc -O2 $INC -DHAVE_PTHREAD -DHAVE_ENDIAN_H -DHAVE_STRUCT_TIMESPEC -D_GNU_SOURCE -DHAVE_SECURE_GETENV -DNDEBUG -DMESA_DEBUG=0 -pthread -sALLOW_MEMORY_GROWTH -sNODERAWFS -sEXIT_RUNTIME -sSTACK_SIZE=4MB -o spirv2nir.js spirv2nir.c $O -lm 2>&1 | grep -v -i warning | head -30
ls -la spirv2nir.js spirv2nir.wasm
