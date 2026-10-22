#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/mesa/src/webgpu/compiler/tools/draw-check.sh [TEST...])
# tests/draw/NAME.{vert,frag}: glslang -> SPIR-V; wgsl-corpus -link (the vertex shader gets the fragment
# shader's interface); draw-run renders both on lavapipe and, as WGSL, on Dawn native, and compares pixels.
set -e
. "$(dirname "$0")/../../../../../build/env.sh"
C=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$NO_BUILD" ] || bash $WT/gpu/webgpu/build/native.sh src/webgpu/compiler/draw-run src/webgpu/compiler/wgsl-corpus > /dev/null
D=$B/draw; rm -rf $D; mkdir -p $D
tests=("$@")
[ ${#tests[@]} = 0 ] && tests=($(cd $C/tests/draw && ls *.frag | sed 's/\.frag$//'))
for t in "${tests[@]}"; do
  for s in frag vert; do
    glslangValidator -V --target-env vulkan1.1 -I$C/tests/draw -o $D/$t-$s.spv $C/tests/draw/$t.$s > $D/$t-$s.glslang 2>&1 || { echo "FAIL $t: glslang $s"; cat $D/$t-$s.glslang; }
  done
  $B/native/src/webgpu/compiler/wgsl-corpus -features -link -o $D $D/$t-frag.spv $D/$t-vert.spv 2>/dev/null
  for f in $D/draw-$t-*; do mv "$f" "$D/${f#$D/draw-}"; done
done
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json $B/native/src/webgpu/compiler/draw-run "${tests[@]/#/$D/}" 2>&1 | grep -v "artificially reduced"
