#!/bin/bash
# Spike S2: Tint (SPIR-V reader + WGSL writer) built natively in the VM. Output ~/rvwasm/webgpu/tint-native/tint
set -e
W=$HOME/rvwasm/webgpu; mkdir -p $W; cd $W
if [ ! -d dawn/.git ]; then
  git clone --depth 1 https://dawn.googlesource.com/dawn dawn
fi
cd dawn
git log -1 --format='dawn %H %cd'
[ -f third_party/abseil-cpp/CMakeLists.txt ] || python3 tools/fetch_dawn_dependencies.py
which cmake ninja
cmake -S . -B $W/tint-native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=ON \
  -DTINT_BUILD_SPV_READER=ON -DTINT_BUILD_WGSL_WRITER=ON -DTINT_BUILD_WGSL_READER=ON -DTINT_BUILD_SPV_WRITER=ON \
  -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF -DTINT_BUILD_HLSL_WRITER=OFF -DTINT_BUILD_MSL_WRITER=OFF \
  -DDAWN_ENABLE_D3D12=OFF -DDAWN_ENABLE_METAL=OFF -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_OPENGLES=OFF \
  -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_NULL=OFF -DDAWN_USE_GLFW=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF \
  -DTINT_BUILD_BENCHMARKS=OFF -DTINT_BUILD_IR_BINARY=OFF
ninja -C $W/tint-native -j6 tint
ls -la $W/tint-native/tint
