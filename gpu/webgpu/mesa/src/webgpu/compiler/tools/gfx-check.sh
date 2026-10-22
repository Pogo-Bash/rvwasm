#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/mesa/src/webgpu/compiler/tools/gfx-check.sh)
# tests/gfx/*.{vert,frag,comp}: glslang -> SPIR-V -> wgsl-corpus (all M4 Max features) -> Tint's validator.
# A static check (no rendering): the WGSL is valid and every construct emitted. Prints ok / FAIL per shader.
set -e
. "$(dirname "$0")/../../../../../build/env.sh"
C=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$NO_BUILD" ] || bash $WT/gpu/webgpu/build/native.sh src/webgpu/compiler/wgsl-corpus > /dev/null
D=$B/gfx; rm -rf $D; mkdir -p $D
fails=0
for f in $C/tests/gfx/*; do
  n=$(basename $f); s=${n//./-}
  glslangValidator -V --target-env vulkan1.1 -o $D/$s.spv $f > $D/$s.glslang 2>&1 || { echo "FAIL $n: glslang"; fails=$((fails+1)); continue; }
  $B/native/src/webgpu/compiler/wgsl-corpus -features -o $D $D/$s.spv 2>/dev/null
  w=$(ls $D/*-$s.wgsl 2>/dev/null | head -1); e=$(ls $D/*-$s.err 2>/dev/null | head -1)
  if [ -n "$e" ]; then echo "FAIL $n: $(tr '\n' ' ' < $e)"; fails=$((fails+1)); continue; fi
  if ! $W/tint-native/tint --format wgsl -o /dev/null $w > $D/$s.tint 2>&1; then
    echo "FAIL $n: tint: $(grep -m1 error $D/$s.tint)"; fails=$((fails+1)); continue; fi
  echo "ok $n"
done
exit $fails
