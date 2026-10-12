#!/bin/bash
# Make what tests/audio/run.sh needs, in build/nm/ (run on the Mac): this tree goes to the build VM (~/wt/NAME, NAME
# the tree's directory name),
# which builds the kernel with the sound card (tools/audio-kernel.sh), rv for wasm, and the test overlay
# (tests/audio/overlay.sh); then those, the runtime as linux-wasm has it patched by this tree's
# tools/patch-runtime.py, and the boot image the dev server serves now (read, not changed) are copied here.
# Nothing is staged.
set -e
cd "$(dirname "$0")/../.."
T=$PWD
NAME=$(basename "$T")
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
SSH="ssh -F $HOME/.lima/rvwasm/ssh.config"
rsync -a --delete --exclude .git --exclude 'tools/chrome' --exclude 'tools/lima' --exclude 'tools/dl' \
	--exclude node_modules --exclude third_party --exclude build -e "$SSH" ./ lima-rvwasm:wt/$NAME/
limactl shell --workdir / rvwasm bash -c "set -e
mkdir -p ~/wt/$NAME/third_party
[ -e ~/wt/$NAME/third_party/berkeley-softfloat-3 ] || ln -s ${RVWASM_MAIN:-$HOME/dev/rvwasm}/third_party/berkeley-softfloat-3 ~/wt/$NAME/third_party/
~/wt/$NAME/tools/audio-kernel.sh ~/wt/$NAME
. ~/rvwasm/env.sh
LLVM=~/rvwasm/linux-wasm/workspace/install/llvm/bin
make -s -j6 -C ~/wt/$NAME/rv O=\$HOME/rvwasm/build-rv-wasm-$NAME CC=\$HOME/wt/$NAME/tools/wasm-cc AR=\$LLVM/llvm-ar 2>&1 |
	grep -v -E 'signature mismatch|^>>> |^\$|not yet stable' || true
RVW=\$HOME/rvwasm/build-rv-wasm-$NAME ~/wt/$NAME/tests/audio/overlay.sh ~/wt/$NAME"
N=build/nm
mkdir -p $N/rt
for f in linux.js linux-worker.js; do
	limactl shell --workdir / rvwasm bash -c "git -C ~/rvwasm/linux-wasm show HEAD:runtime/$f" > $N/rt/$f
done
python3 tools/patch-runtime.py $N/rt > /dev/null
limactl shell --workdir / rvwasm bash -c 'cat ~/rvwasm/kernels/vmlinux-audio.wasm' > $N/vmlinux.wasm
limactl shell --workdir / rvwasm bash -c 'cat ~/rvwasm/www/initramfs-wasm64_nommu.cpio.gz' > $N/initramfs.cpio.gz
limactl shell --workdir / rvwasm bash -c 'cat ~/rvwasm/audio-test/overlay.cpio.gz' > $N/overlay.cpio.gz
ls -la $N $N/rt
