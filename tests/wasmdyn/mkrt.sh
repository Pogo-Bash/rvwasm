#!/bin/bash
# The runtime the tests use: linux-wasm's own (pristine, from the build VM's checkout, cached), patched by
# tools/patch-runtime.py as every runtime is. In build/wasmdyn/rt.
cd "$(dirname "$0")/../.."
B=build/wasmdyn
mkdir -p $B/pristine
if [ ! -f $B/pristine/linux-worker.js ]; then
	for f in linux-worker.js linux.js; do
		ssh -F $HOME/.lima/rvwasm/ssh.config lima-rvwasm "cd ~/rvwasm/linux-wasm && git show HEAD:runtime/$f" > $B/pristine/$f
	done
fi
rm -rf $B/rt; mkdir -p $B/rt; cp $B/pristine/linux-worker.js $B/pristine/linux.js $B/rt/
python3 tools/patch-runtime.py $B/rt > /dev/null
