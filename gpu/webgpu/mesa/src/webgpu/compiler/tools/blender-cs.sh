#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/mesa/src/webgpu/compiler/tools/blender-cs.sh [OUTDIR])
# Blender's compute shaders that only use buffers, run on lavapipe (SPIR-V) and on Dawn native (our WGSL) with
# the same pattern-filled buffers, compared word by word (cs-run -pattern). OUTDIR is a corpus.sh output
# (default $B/tests/corpus, made with -features -remap).
set -e
. "$(dirname "$0")/../../../../../build/env.sh"
O=${1:-$B/tests/corpus}
[ -n "$NO_BUILD" ] || bash $WT/gpu/webgpu/build/native.sh src/webgpu/compiler/cs-run > /dev/null
# a private copy: corpus.sh (compiler.test) recreates OUTDIR
R=$B/blender-cs; rm -rf $R; mkdir -p $R
spv=()
for f in $O/*.wgsl; do
  grep -q "@compute" $f || continue
  n=$(basename $f .wgsl); d=${n%-dump*}
  cp $f ${f%.wgsl}.bind $R/; cp $W/out/$d/${n##*-}.spv $R/$n.spv; spv+=($R/$n.spv)
done
# one process per shader, 300 s each: some shaders loop for a long time on pattern data
ok=0; failed=0; slow=0; skipped=0
for f in "${spv[@]}"; do
  out=$(VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json timeout 300 $B/native/src/webgpu/compiler/cs-run -pattern -groups 1 $f 2>&1 | grep -v "artificially reduced\|^cs-run:" || true)
  if [ -z "$out" ]; then echo "slow $(basename $f): over 300 s"; slow=$((slow+1)); continue; fi
  echo "$out"
  case "$out" in ok*) ok=$((ok+1));; skip*) skipped=$((skipped+1));; *) failed=$((failed+1));; esac
done
echo "blender-cs: $ok equal, $failed differ, $slow over 300 s, $skipped skipped"
