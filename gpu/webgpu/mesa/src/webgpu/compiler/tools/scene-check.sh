#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/mesa/src/webgpu/compiler/tools/scene-check.sh [TEST...])
# T3b's graphics runtime checks: tests/scene/NAME.{vert,frag,scene} -> glslang -> wgsl-corpus -layout -legalize
# -features -link -scene (the fragment shader first) -> scene-run draws them on lavapipe as SPIR-V with the
# scene's real vertex input and draw, and on Dawn native as our WGSL with what the passes asked for.
set -e
. "$(dirname "$0")/../../../../../build/env.sh"
C=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$NO_BUILD" ] || bash $WT/gpu/webgpu/build/native.sh src/webgpu/compiler/scene-run src/webgpu/compiler/wgsl-corpus > /dev/null
D=$B/scene; rm -rf $D; mkdir -p $D/spv
tests=("$@")
[ ${#tests[@]} = 0 ] && tests=($(cd $C/tests/scene && ls *.scene | sed 's/\.scene$//'))
run=()
for t in "${tests[@]}"; do
  ok=1
  sc=$C/tests/scene/$t.scene
  stages=(frag); { [ -e $C/tests/scene/$t.geom ] || grep -q "^spirv geom" $sc; } && stages+=(geom)
  [ -e $C/tests/scene/$t.tese ] && stages+=(tese tesc)   # (the TES gets the fragment shader's interface)
  stages+=(vert)
  for s in "${stages[@]}"; do
    # "spirv STAGE PATH": a stage from the Blender corpus ($W/out/PATH.spv)
    sp=$(sed -n "s|^spirv $s ||p" $sc)
    if [ -n "$sp" ]; then cp $W/out/$sp.spv $D/spv/$t-$s.spv; continue; fi
    glslangValidator -V --target-env vulkan1.2 -I$C/tests/scene -o $D/spv/$t-$s.spv $C/tests/scene/$t.$s > $D/$t-$s.glslang 2>&1 || { echo "FAIL $t: glslang $s: $(grep -m1 ERROR $D/$t-$s.glslang)"; ok=0; }
  done
  [ $ok = 1 ] || continue
  cp $C/tests/scene/$t.scene $D/
  $B/native/src/webgpu/compiler/wgsl-corpus -layout -legalize -features -link -scene $D/$t.scene -o $D $(for s in "${stages[@]}"; do echo $D/spv/$t-$s.spv; done) > $D/$t.corpus 2>&1 || { echo "FAIL $t: wgsl-corpus: $(grep -m1 -i error $D/$t.corpus)"; continue; }
  for f in $D/spv-$t-*; do mv "$f" "$D/${f#$D/spv-}"; done
  for s in "${stages[@]}"; do cp $D/spv/$t-$s.spv $D/; done
  for s in "${stages[@]}"; do
    if [ -e $D/$t-$s.err ]; then echo "FAIL $t: $s: $(tr '\n' ' ' < $D/$t-$s.err)"; ok=0; continue; fi
    for w in $D/$t-$s.wgsl $D/$t-$s.gs-*.wgsl $D/$t-$s.vs-compute.wgsl; do
      [ -e $w ] || continue
      $W/tint-native/tint --format wgsl -o /dev/null $w > $D/$t-$s.tint 2>&1 || { echo "FAIL $t: tint $(basename $w): $(grep -m1 error $D/$t-$s.tint)"; ok=0; }
    done
  done
  for w in $D/$t-geom*.wgsl; do   # (transform feedback's passthrough GS has no geom stage of its own)
    [ -e $w ] || continue
    $W/tint-native/tint --format wgsl -o /dev/null $w > $D/$t-geom.tint 2>&1 || { echo "FAIL $t: tint $(basename $w): $(grep -m1 error $D/$t-geom.tint)"; ok=0; }
  done
  [ $ok = 1 ] && run+=($D/$t)
done
[ ${#run[@]} = 0 ] && exit 1
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json $B/native/src/webgpu/compiler/scene-run "${run[@]}" 2>&1 | grep -v "artificially reduced"
