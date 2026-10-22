#!/bin/bash
# Spike S4: Mesa's shader compiler (NIR + spirv_to_nir) built with Emscripten, and natively for comparison.
#   build.sh   (in the VM; Mesa source in ~/rvwasm/webgpu/mesa-25.0.7)
set -e
M=$HOME/rvwasm/webgpu/mesa-25.0.7; S=$HOME/rvwasm/webgpu/mesa-wasm-spike
OPTS="-Dplatforms= -Dgallium-drivers= -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false
 -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dshared-glapi=disabled -Dzstd=disabled -Dexpat=disabled
 -Dlibunwind=disabled -Dvalgrind=disabled -Dbuild-tests=false -Dshader-cache=disabled -Dvideo-codecs=
 -Dlmsensors=disabled -Dxmlconfig=disabled -Dbuildtype=release -Db_ndebug=true"
source ~/emsdk/emsdk_env.sh > /dev/null 2>&1
cd $M
[ -d $S/native ] || meson setup $S/native $OPTS > $S-native-setup.log 2>&1 || { tail -30 $S-native-setup.log; exit 1; }
ninja -C $S/native -j6 src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/util/libmesa_util.a src/compiler/libcompiler.a 2>&1 | tail -3
[ -d $S/wasm ] || meson setup $S/wasm --cross-file $S/emscripten.cross $OPTS > $S-wasm-setup.log 2>&1 || { tail -30 $S-wasm-setup.log; exit 1; }
ninja -C $S/wasm -j6 src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/util/libmesa_util.a src/compiler/libcompiler.a 2>&1 | tail -3
ls -la $S/*/src/compiler/spirv/libvtn.a $S/*/src/compiler/nir/libnir.a
