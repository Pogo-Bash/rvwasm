#!/bin/bash
# (In the VM) write the build tree's changes to virglrenderer back as gpu/webgpu/vkr/virglrenderer-page.diff
# of the VM copy; copy it to the Mac worktree afterwards (gpu/webgpu/build/vm.sh syncs Mac -> VM only).
set -e
. "$(dirname "$0")/../build/env.sh"
VT=$W/vkr-trees/$NAME
git -C $VT add -N src && git -C $VT diff -- src > $WT/gpu/webgpu/vkr/virglrenderer-page.diff
sha1sum < $WT/gpu/webgpu/vkr/virglrenderer-page.diff | cut -c1-40 | sed "s/^/$(cut -d' ' -f1-2 $VT/.rvgpu-stamp) /" > $VT/.rvgpu-stamp
wc -l $WT/gpu/webgpu/vkr/virglrenderer-page.diff
