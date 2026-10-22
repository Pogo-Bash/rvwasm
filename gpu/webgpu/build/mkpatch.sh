#!/bin/bash
# (In the VM) write this build's Mesa tree changes (tracked files only; src/webgpu is ours and not in it)
# to patches/mesa-page.diff of the VM copy, and print it; copy it back to the Mac worktree with
#   ssh -F ~/.lima/rvwasm/ssh.config lima-rvwasm cat wt/NAME/patches/mesa-page.diff > patches/mesa-page.diff
set -e
. "$(dirname "$0")/env.sh"
git -C $TREE diff $PIN -- . ':!src/webgpu' > $WT/patches/mesa-page.diff
h="$PIN $(sha1sum < $WT/patches/mesa-page.diff | cut -c1-40)"
echo "$h" > $TREE/.rvgpu-stamp
git -C $TREE diff --stat $PIN -- . ':!src/webgpu'
