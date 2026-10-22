#!/bin/bash
# Spike S5: Dawn native (webgpu.h on Vulkan, here on lavapipe) in the VM: the native test bed for a Vulkan driver
# written against webgpu.h (same API as Emscripten's emdawnwebgpu in the page).
set -e
W=$HOME/rvwasm/webgpu; cd $W/dawn
cmake -S . -B $W/dawn-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF \
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DDAWN_ENABLE_VULKAN=ON -DDAWN_ENABLE_OPENGLES=OFF \
  -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_NULL=ON -DDAWN_USE_GLFW=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF \
  -DDAWN_BUILD_MONOLITHIC_LIBRARY=SHARED -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DDAWN_SUPPORTS_CXX_MODULES=OFF -DTINT_BUILD_BENCHMARKS=OFF > $W/dawn-native-cmake.log 2>&1 || { tail -20 $W/dawn-native-cmake.log; exit 1; }
ninja -C $W/dawn-native -j6 webgpu_dawn 2>&1 | tail -2
ls -la $W/dawn-native/src/dawn/native/libwebgpu_dawn* 2>/dev/null || find $W/dawn-native -name "libwebgpu_dawn*"
