#!/bin/bash
# On the Mac: vfront in the page. Builds rvgpu.wasm (wasm.sh in the VM), copies it with rvgpu-worker.js,
# rvgpu-vfront.html and a capture (default glm-small, from the VM's ~/rvwasm/gpu-t6/cap) into build/vfront-page,
# and replays the capture in the test browser (tools/with-browser.sh, one at a time): vfront on zink on wgvk on
# Chrome's WebGPU. Prints the page log and the result; screenshot build/vfront-page/replay.png.
#   page-replay.sh [--no-build] [CAPTURE_NAME]
set -e
WT=$(cd "$(dirname "$0")/../../../../../../.." && pwd)
NAME=${GPU_BUILD:-$(basename "$WT")}
cd "$WT"
if [ "$1" = --no-build ]; then shift; else JOBS=${JOBS:-4} gpu/webgpu/build/vm.sh gpu/webgpu/build/wasm.sh | tail -3; fi
cap=${1:-glm-small}
D=$WT/build/vfront-page
mkdir -p $D/rvgpu
S="ssh -F $HOME/.lima/rvwasm/ssh.config"
for f in rvgpu.js rvgpu.wasm; do
	scp -q -F $HOME/.lima/rvwasm/ssh.config lima-rvwasm:rvwasm/webgpu/b/$NAME/wasm/src/webgpu/rvgpu/$f $D/rvgpu/$f
done
[ -f $D/$cap.vfcap ] || scp -q -F $HOME/.lima/rvwasm/ssh.config lima-rvwasm:rvwasm/gpu-t6/cap/$cap.vfcap $D/$cap.vfcap
cp tools/www/rvgpu-worker.js tools/www/rvgpu-vfront.html $D/
sed -i '' "s/|| \"glm-small.vfcap\"/|| \"$cap.vfcap\"/" $D/rvgpu-vfront.html
SMOKE_PAGE=rvgpu-vfront.html SMOKE_WAIT_S=${WAIT_S:-300} tools/with-browser.sh "$NAME-vfront" \
	node gpu/webgpu/build/smoke-browser.js $D $D/replay.png
