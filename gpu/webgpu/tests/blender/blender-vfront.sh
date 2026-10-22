#!/bin/bash
# (VM, outside the chroot) Blender 4.3.2 in the trixie chroot through vfront-vtest on wgvk on Dawn (the guest's
# Mesa 25.0.7 virgl driver, GALLIUM_DRIVER=virpipe, over a vtest socket): the real shipping GL path.
#   VFRONT_BUILD=vk-03 blender-vfront.sh MODE TAG [lvp]        MODE = workbench | eevee | passes | ui
# Output: ~/rvwasm/gpu-t6/blender/TAG-MODE (/t6/blender in the chroot). With "lvp" as the third word the server
# runs zink on lavapipe instead of wgvk. The vfront-vtest server leaks (10 GB in 1760 piglit tests): main-run.sh
# restarts it past MAXRSS_MB (here 5000), which would end a render, so the log says when that happened.
# EEVEE needs SHADOW_POOL=64 (Dawn's 128 MB binding limit); SAMPLES=n as in blender-run.sh.
S=$(cd "$(dirname "$0")" && pwd)
V=$S/../../mesa/src/webgpu/vfront/scripts
mode=${1:-workbench}; tag=${2:-x}; vk=wgvk; [ "$3" = lvp ] && vk=lvp
G=$HOME/rvwasm/gpu-t6/blender
mkdir -p $G
cp $S/blender-run.sh $S/passes.py $S/imgdiff.py $G/
env="OUT=/t6/blender SAMPLES=$SAMPLES SHADOW_POOL=$SHADOW_POOL TMO=${TMO:-1500}"
export MAXRSS_MB=${MAXRSS_MB:-5000} RESTART=${RESTART:-0}
# (peak resident size of the server, sampled while it runs)
( pk=0; while sleep 0.5; do p=$(pgrep -f "bl-$tag-$mode-vfront-vtest" | head -1); [ -n "$p" ] || continue
  r=$(awk '/VmRSS/ {print int($2 / 1024)}' /proc/$p/status 2>/dev/null); [ "${r:-0}" -gt $pk ] && { pk=$r; echo $pk > /tmp/bl-$tag-$mode.peak; }; done ) &
sp=$!
bash $V/main-run.sh --vk $vk bl-$tag-$mode "export GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1 LIBGL_DEBUG=quiet; $env bash /t6/blender/blender-run.sh $mode $tag"
kill $sp 2>/dev/null
echo "server peak RSS: $(cat /tmp/bl-$tag-$mode.peak 2>/dev/null || echo '<0.5 s sample') MB"
grep -E "restarting|died" $HOME/rvwasm/gpu-t6/log/bl-$tag-$mode-server.log | head
