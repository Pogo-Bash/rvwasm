#!/bin/bash
# On the Mac: build rvgpu.wasm (wasm.sh in the VM), copy it with tools/www/rvgpu-worker.js and
# rvgpu-smoke.html into a directory of this worktree (build/rvgpu-page, untracked), and run the page smoke test
# in the test browser (one at a time: tools/with-browser.sh). Screenshot: build/rvgpu-page/smoke.png.
#   gpu/webgpu/build/smoke-page.sh [--no-build] [TEST]      (TEST: clear (default) or draw; screenshot TEST.png)
set -e
WT=$(cd "$(dirname "$0")/../../.." && pwd)
NAME=${GPU_BUILD:-$(basename "$WT")}
cd "$WT"
build=1
[ "$1" = --no-build ] && { build=; shift; }
TEST=${1:-clear}
[ -z "$build" ] || gpu/webgpu/build/vm.sh gpu/webgpu/build/wasm.sh | tail -3
D=$WT/build/rvgpu-page
mkdir -p $D/rvgpu
for f in rvgpu.js rvgpu.wasm; do
	scp -q -F $HOME/.lima/rvwasm/ssh.config lima-rvwasm:rvwasm/webgpu/b/$NAME/wasm/src/webgpu/rvgpu/$f $D/rvgpu/$f
done
cp tools/www/rvgpu-worker.js tools/www/rvgpu-smoke.html $D/
shot=$D/smoke.png
[ $TEST = clear ] || shot=$D/smoke-$TEST.png
tools/with-browser.sh "$NAME-smoke" env SMOKE_RELOAD=${SMOKE_RELOAD:-} node gpu/webgpu/build/smoke-browser.js $D $shot $TEST
