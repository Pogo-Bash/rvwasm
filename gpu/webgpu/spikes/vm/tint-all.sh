#!/bin/bash
# Spike S2: every SPIR-V module zink made for Blender, through Tint's SPIR-V reader -> WGSL writer.
T=$HOME/rvwasm/webgpu/tint-native/tint
O=$HOME/rvwasm/webgpu/out
for d in "$@"; do
  ok=0; bad=0; rm -rf $O/wgsl-$d; mkdir -p $O/wgsl-$d
  for f in $O/$d/dump*.spv; do
    b=$(basename $f .spv)
    if $T --format wgsl $f -o $O/wgsl-$d/$b.wgsl > $O/wgsl-$d/$b.err 2>&1; then ok=$((ok+1)); rm -f $O/wgsl-$d/$b.err; else bad=$((bad+1)); fi
  done
  echo "$d: ok $ok failed $bad"
  cat $O/wgsl-$d/*.err 2>/dev/null | grep -E "error" | sed -E 's/^[^:]*:[0-9]+:[0-9]+ //; s/id [0-9]+/id N/g; s/%[A-Za-z0-9_]+/%X/g; s/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -12
done
