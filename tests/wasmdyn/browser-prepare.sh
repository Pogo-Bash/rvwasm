#!/bin/bash
# Prepare what the owner's browser test needs (docs/wasm-dynamic-linking.md, "Owner's browser test"), in build/wasmdyn/browser/:
# the patched runtime, this tree's dev.html, the dev server's boot image with the tests (/wasmdyn: ld-wasm, libfoo,
# qtcore-hello, libQt6Core.so.6) appended as ?image=wasmdyn, and dl/ (tools/dl-manifest.py: libs.json and the libraries
# under their content keys). Run run.sh once first (it fetches the build VM's out/ and builds the runtime).
# Then:  tests/wasmdyn/browser-serve.py build/wasmdyn/browser   and open the URL it prints.
# KCALC=1: the KCalc test instead (the manual test sheets): image "kcalc" = the boot image + build/wasmdyn/kc.cpio.gz
# (/wasmdyn/{ld-wasm,kcalc}, the 30 library names in /usr/lib, plugins), dl/ = kc-dl/ (35 libraries, all preloaded). Run
# tests/wasmdyn/run-kcalc.sh once first (it builds both). It needs the dev server's disk "daily" (?disk=daily).
set -e
cd "$(dirname "$0")/../.."
B=build/wasmdyn
D=$B/browser
rm -rf $D; mkdir -p $D
cp $B/rt/linux.js $B/rt/linux-worker.js $D/
cp tools/www/dev.html $D/
S=http://localhost:8000
curl -sf -o $B/initramfs.cpio.gz $S/initramfs-wasm64_nommu.cpio.gz
# (two gzip'd cpio archives one after the other are one initramfs, as node-machine --initramfs does)
if [ -n "$KCALC" ]; then
	[ -f $B/kc.cpio.gz ] && [ -f $B/kc-dl/libs.json ] || { echo "run tests/wasmdyn/run-kcalc.sh first"; exit 1; }
	cat $B/initramfs.cpio.gz $B/kc.cpio.gz > $D/initramfs-kcalc-wasm64_nommu.cpio.gz
	cp -R $B/kc-dl $D/dl; rm -f $D/dl/.fp
else
	cat $B/initramfs.cpio.gz $B/wasmdyn.cpio.gz > $D/initramfs-wasmdyn-wasm64_nommu.cpio.gz
	python3 tools/dl-manifest.py --out $D/dl $B/out/libQt6Core.so.6 --preload libQt6Core.so.6
fi
ls -la $D $D/dl
