#!/bin/bash
# Run the dynamic-linking prototype under node-machine (on the Mac): the dev server's kernel, initramfs
# and runtime (the runtime patched by tools/patch-runtime.py, dynamic libraries on), and out/ (from the build VM, build.sh) as
# /wasmdyn in one more initramfs.
#   tests/wasmdyn/run.sh 'COMMAND' [TIMEOUT_MS]
#   e.g. tests/wasmdyn/run.sh 'cd /wasmdyn && ./ld-wasm ./main1'
#   HOOK=tests/wasmdyn/dl-hook.js prints the module cache's statistics after the command; PRELOAD=FILE.json compiles libraries first.
set -e
cd "$(dirname "$0")/../.."
B=build/wasmdyn
mkdir -p $B/rt $B/root/wasmdyn
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
rsync -a -e "ssh -F $HOME/.lima/rvwasm/ssh.config" lima-rvwasm:wt/wasmdyn/tests/wasmdyn/out/ $B/out/
cp $B/out/ld-wasm $B/out/libfoo.so $B/out/main1 $B/out/main2 $B/out/main3 $B/out/main4 $B/out/main5 $B/out/libbig.so $B/out/libbig-js.so $B/out/libbig-noexp.so $B/out/libbigcall.so $B/out/bench $B/out/bench-static $B/out/qtcore-hello $B/out/qtcore-hello-static $B/out/libQt6Core.so.6 $B/out/hello $B/out/hello-static $B/out/timeit $B/root/wasmdyn/
(cd $B/root && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2>/dev/null | gzip -1 > ../wasmdyn.cpio.gz)
S=http://localhost:8000
[ -f $B/vmlinux.wasm ] || curl -sf -o $B/vmlinux.wasm $S/vmlinux-wasm64_nommu.wasm
[ -f $B/initramfs.cpio.gz ] || curl -sf -o $B/initramfs.cpio.gz $S/initramfs-wasm64_nommu.cpio.gz
tests/wasmdyn/mkrt.sh
# One node-machine at a time on this Mac (other agents use it too).
while pgrep -f "^node .*tools/[n]ode-machine.js" > /dev/null; do sleep 10; done
exec perl -e 'alarm shift; exec @ARGV' 300 node --experimental-wasm-memory64 tools/node-machine.js --runtime $B/rt \
	--kernel $B/vmlinux.wasm --initramfs $B/initramfs.cpio.gz --initramfs $B/wasmdyn.cpio.gz \
	--timeout "${2:-60000}" --query "?dl=${DL-1}" ${HOOK:+--hook $HOOK} ${PRELOAD:+--dl-preload $PRELOAD} ${MANIFEST:+--dl-manifest $MANIFEST} ${V:+--verbose} "$1"
