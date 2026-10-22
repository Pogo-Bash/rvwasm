#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/mesa/src/webgpu/compiler/tools/legal-check.sh [TEST...])
# T3b's static checks: each tests/legal/TEST.{comp,vert,frag} -> glslang (Vulkan 1.2) -> wgsl-corpus with
# T1's layout rules and the legalisation passes (-layout -legalize -features) -> Tint's WGSL validator.
# Prints "ok NAME" or "FAIL NAME: reason". Outputs (.wgsl, .bind, .err) in $B/legal.
# Then the compute tests run on lavapipe as SPIR-V and on Dawn native as our WGSL (cs-run), buffers compared.
set -e
. "$(dirname "$0")/../../../../../build/env.sh"
C=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$NO_BUILD" ] || bash $WT/gpu/webgpu/build/native.sh src/webgpu/compiler/wgsl-corpus src/webgpu/compiler/cs-run > /dev/null
D=$B/legal; rm -rf $D; mkdir -p $D/spv
tests=("$@")
[ ${#tests[@]} = 0 ] && tests=($(cd $C/tests/legal && ls | sed 's/\.[a-z]*$//' | sort -u))
pass=0; fail=0
for t in "${tests[@]}"; do
  src=$(ls $C/tests/legal/$t.* | head -1)
  if ! glslangValidator -V --target-env vulkan1.2 -o $D/spv/$t.spv $src > $D/$t.glslang 2>&1; then
    echo "FAIL $t: glslang: $(grep -m1 ERROR $D/$t.glslang)"; fail=$((fail+1)); continue
  fi
  # "// legal-check: options ..." in a test: more wgsl-corpus options (shader key fields)
  extra=($(sed -n 's|^// legal-check: options ||p' $src))
  $B/native/src/webgpu/compiler/wgsl-corpus -layout -legalize -features "${extra[@]}" -o $D $D/spv/$t.spv > $D/$t.corpus 2>&1 || { echo "FAIL $t: wgsl-corpus crashed: $(grep -m1 -i error $D/$t.corpus)"; fail=$((fail+1)); continue; }
  if [ -e $D/spv-$t.err ]; then
    echo "FAIL $t: $(tr '\n' ' ' < $D/spv-$t.err)"; fail=$((fail+1)); continue
  fi
  if ! $W/tint-native/tint --format wgsl -o /dev/null $D/spv-$t.wgsl > $D/$t.tint 2>&1; then
    echo "FAIL $t: tint: $(grep -m1 error $D/$t.tint)"; fail=$((fail+1)); continue
  fi
  echo "ok $t"; pass=$((pass+1))
  grep -q "legal-check: static" $src && continue
  case $src in *.comp) mkdir -p $D/run; cp $D/spv/$t.spv $D/run/; for e in wgsl bind; do cp $D/spv-$t.$e $D/run/$t.$e; done;; esac
done
echo "legal-check: $pass ok, $fail failed (emit + Tint)"
if [ -d $D/run ]; then
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json $B/native/src/webgpu/compiler/cs-run $D/run/*.spv 2>&1 | grep -v "artificially reduced" || fail=$((fail+1))
fi
[ $fail = 0 ]
