#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/mesa/src/webgpu/compiler/tools/cs-check.sh [-wgsl] [TEST...])
# Each tests/cs/TEST.comp: glslang -> SPIR-V, then cs-check runs it on lavapipe and, through our WGSL, on
# Dawn native (on lavapipe too), and compares the buffers word by word. Prints "ok NAME" / "FAIL NAME".
set -e
. "$(dirname "$0")/../../../../../build/env.sh"
C=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$NO_BUILD" ] || bash $WT/gpu/webgpu/build/native.sh src/webgpu/compiler/cs-run src/webgpu/compiler/wgsl-corpus > /dev/null
D=$B/cs; mkdir -p $D
opts=(); tests=()
for a in "$@"; do case $a in -*) opts+=("$a");; *) tests+=("$a");; esac; done
[ ${#tests[@]} = 0 ] && tests=($(cd $C/tests/cs && ls *.comp | sed 's/\.comp$//'))
spv=()
for t in "${tests[@]}"; do
  glslangValidator -V --target-env vulkan1.1 -o $D/$t.spv $C/tests/cs/$t.comp > $D/$t.glslang 2>&1 || { echo "FAIL $t: glslang"; cat $D/$t.glslang; continue; }
  spv+=($D/$t.spv)
done
# cs-run reads NAME.wgsl / .bind / .err next to NAME.spv (wgsl-corpus names them DIR-NAME: rename)
rm -f $D/*.wgsl $D/*.bind $D/*.err
$B/native/src/webgpu/compiler/wgsl-corpus -features -o $D "${spv[@]}" 2>/dev/null
for f in $D/cs-*.wgsl $D/cs-*.bind $D/cs-*.err; do [ -e "$f" ] && mv "$f" "$D/${f#$D/cs-}"; done
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json $B/native/src/webgpu/compiler/cs-run "${opts[@]}" "${spv[@]}" 2>&1 | grep -v "artificially reduced"
