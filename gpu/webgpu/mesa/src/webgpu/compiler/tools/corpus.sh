#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/mesa/src/webgpu/compiler/tools/corpus.sh OUTDIR [opts])
# Blender 4.3.2's 177 zink SPIR-V modules (wgmask P4 profile, spike S2) through wgsl-corpus, then Tint's WGSL
# validator. Prints: emitted / blocked counts, Tint valid / invalid, blockers by tag, Tint errors by kind.
# Builds wgsl-corpus first (native.sh target). Options go to wgsl-corpus (-remap, -features).
set -e
. "$(dirname "$0")/../../../../../build/env.sh"
O=${1:?OUTDIR}; shift
[ -n "$NO_BUILD" ] || bash $WT/gpu/webgpu/build/native.sh src/webgpu/compiler/wgsl-corpus > /dev/null
TINT=$W/tint-native/tint; S=$W/out
rm -rf $O; mkdir -p $O
$B/native/src/webgpu/compiler/wgsl-corpus "$@" -o $O $S/spv-ui-p4/*.spv $S/spv-eevee-p4/*.spv $S/spv-workbench-p4/*.spv 2>&1 | tail -1
ok=0; bad=0
for f in $O/*.wgsl; do
  [ -e "$f" ] || continue
  if $TINT --format wgsl -o /dev/null "$f" > "${f%.wgsl}.tint" 2>&1; then ok=$((ok+1)); rm -f "${f%.wgsl}.tint"; else bad=$((bad+1)); fi
done
# (geometry shaders through libpoly emit a module per part: NAME.wgsl, the rast part, counts as the module)
parts=$(ls $O/*.gs-*.wgsl $O/*.vs-compute.wgsl 2>/dev/null | wc -l)
echo "modules: $(ls $O/*.wgsl 2>/dev/null | grep -v '\.gs-\|\.vs-compute\|\.tess-' | wc -l) emitted, $(ls $O/*.err 2>/dev/null | wc -l) blocked; Tint: $ok valid, $bad invalid (WGSL files, $parts of them extra libpoly parts)"
echo "--- blockers (modules per tag)"
for f in $O/*.err; do [ -e "$f" ] && sed -E 's/\] .*/]/' "$f" | sort -u; done | sort | uniq -c | sort -rn
echo "--- blockers (modules blocked by this tag alone)"
for f in $O/*.err; do [ -e "$f" ] || continue; t=$(sed -E 's/\] .*/]/' "$f" | sort -u); [ $(echo "$t" | wc -l) = 1 ] && echo "$t"; done | sort | uniq -c | sort -rn
echo "--- emitted modules over a WebGPU limit of the M4 Max adapter (modules per tag)"
for f in $O/*.limits; do [ -e "$f" ] && sed -E 's/\] .*/]/' "$f" | sort -u; done | sort | uniq -c | sort -rn
echo "--- Tint errors (first error per module)"
for f in $O/*.tint; do [ -e "$f" ] && grep -m1 "error" "$f"; done | sed -E 's/^[^ ]* //; s/[0-9]+/N/g; s/'"'"'[^'"'"']*'"'"'/X/g' | sort | uniq -c | sort -rn | head -30
true
