#!/bin/bash
# On the Mac: copy this worktree into the build VM (it mounts only the main checkout) as ~/wt/NAME there,
# then run a command in that copy. NAME = the worktree's directory name (or $GPU_BUILD).
#   gpu/webgpu/build/vm.sh gpu/webgpu/build/native.sh        (paths relative to the worktree)
#   gpu/webgpu/build/vm.sh --sync-only
# rsync keeps modification times, so ninja rebuilds only what changed (after a merge or checkout, touch the
# sources that changed, or a build may test stale code).
# The VM copy has no .git, so the build stamp's source (wasm.sh -> tools/rvgpu-stamp.js) is read here and sent
# as RVGPU_BRANCH / RVGPU_COMMIT / RVGPU_DIRTY ("dirty": uncommitted changes under gpu/webgpu or patches).
set -e
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
WT=$(cd "$(dirname "$0")/../../.." && pwd)
NAME=${GPU_BUILD:-$(basename "$WT")}
cd "$WT"
export RVGPU_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
export RVGPU_COMMIT=$(git rev-parse HEAD 2>/dev/null || echo unknown)
if [ -n "$(git status --porcelain -- gpu/webgpu patches 2>/dev/null)" ]; then export RVGPU_DIRTY=true; else export RVGPU_DIRTY=false; fi
limactl shell --workdir / rvwasm mkdir -p ~/wt/$NAME
rsync -a --delete --exclude .git --exclude 'tools/chrome' --exclude 'tools/lima' --exclude 'tools/dl' \
	--exclude node_modules --exclude third_party --exclude '/build' --exclude '/linux-wasm' \
	-e "ssh -F $HOME/.lima/rvwasm/ssh.config" ./ lima-rvwasm:wt/$NAME/
[ "$1" = --sync-only ] && exit 0
cmd=$1; shift
# (these variables of the Mac's environment go along)
pass=()
for v in NO_LINK NO_BUILD GPU_NATIVE GPU_PLATFORMS RVGPU_BRANCH RVGPU_COMMIT RVGPU_DIRTY $(env | grep -o '^WGVK_[A-Z_]*'); do [ -n "${!v}" ] && pass+=("$v=${!v}"); done
exec limactl shell --workdir / rvwasm env GPU_BUILD=$NAME JOBS=${JOBS:-6} "${pass[@]}" bash ~/wt/$NAME/$cmd "$@"
