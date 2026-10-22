#!/bin/bash
# (In the VM) A second corpus: SPIRV-Cross's GLSL test shaders (shaders/{comp,frag,vert,vulkan,desktop-only,flatten}),
# compiled with glslang for Vulkan, then through wgsl-corpus (-features -remap) and Tint. Same summary as corpus.sh.
#   tools/xcorpus.sh OUTDIR [wgsl-corpus options, default -features -remap]   (clones SPIRV-Cross into $W/spirv-cross on first use)
set -e
. "$(dirname "$0")/../../../../../build/env.sh"
O=${1:?OUTDIR}; shift
OPTS=("$@"); [ ${#OPTS[@]} -gt 0 ] || OPTS=(-features -remap)
X=$W/spirv-cross
[ -d $X ] || git clone -q --depth 1 https://github.com/KhronosGroup/SPIRV-Cross.git $X
[ -n "$NO_BUILD" ] || bash $WT/gpu/webgpu/build/native.sh src/webgpu/compiler/wgsl-corpus > /dev/null
rm -rf $O; mkdir -p $O/spv
n=0; bad=0
for f in $(cd $X/shaders && find comp frag vert vulkan desktop-only flatten -type f \( -name "*.comp" -o -name "*.frag" -o -name "*.vert" \) | sort); do
  s=$(echo $f | tr '/' '_' | sed 's/\./-/g')
  if glslangValidator -V --target-env vulkan1.1 -o $O/spv/$s.spv $X/shaders/$f > /dev/null 2>&1; then n=$((n+1)); else bad=$((bad+1)); rm -f $O/spv/$s.spv; fi
done
echo "glslang: $n compiled, $bad refused"
$B/native/src/webgpu/compiler/wgsl-corpus "${OPTS[@]}" -o $O $O/spv/*.spv 2>&1 | tail -1
ok=0; inv=0
for f in $O/*.wgsl; do
  if $W/tint-native/tint --format wgsl -o /dev/null "$f" > "${f%.wgsl}.tint" 2>&1; then ok=$((ok+1)); rm -f "${f%.wgsl}.tint"; else inv=$((inv+1)); fi
done
echo "modules: $(ls $O/*.wgsl 2>/dev/null | wc -l) emitted, $(ls $O/*.err 2>/dev/null | wc -l) blocked; Tint: $ok valid, $inv invalid"
echo "--- blockers (modules per tag)"
for f in $O/*.err; do [ -e "$f" ] && sed -E 's/\] .*/]/' "$f" | sort -u; done | sort | uniq -c | sort -rn
echo "--- Tint errors"
for f in $O/*.tint; do [ -e "$f" ] && echo "$(basename $f): $(grep -m1 error $f | sed 's/^[^ ]* //')"; done
true
