#!/bin/bash
# (In the VM) The SPIR-V of the smoke-draw shaders as C arrays (spv/*.h), for the page build, which has no
# glslang: run after changing tri.vert, color.frag, tex.frag, ubo.frag or square.comp, and commit spv/.
set -e
cd "$(dirname "$0")"
mkdir -p spv
for f in tri.vert color.frag tex.frag ubo.frag square.comp; do
	n=spv_$(echo $f | tr . _)
	glslangValidator -V --target-env vulkan1.1 --vn $n -o spv/$f.h $f > /dev/null
done
