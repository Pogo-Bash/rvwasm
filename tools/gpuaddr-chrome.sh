#!/bin/bash
# gpuaddr-chrome: the release build in the test browser at ?mem=8192 with a GL program drawing, while the machine's free
# memory below FLOOR is taken (gpu/server/ballast.js), so that every address the GPU server, the window server and the
# page's executor deal with lies above 2 GiB or 4 GiB (what a big computer's default memory does once it has been in use
# a while). Passes when no Worker threw (tools/desktop-drive.js --worker-errors), the program's window is drawn and the
# ballast is intact (nothing wrote to a wrapped address).
#
#   tools/gpuaddr-chrome.sh [--before] [--floor GIB] [--mem MIB]
#
# DIST (default dist/): a release build (docker/release.sh dist: www/ and disk/); the page files (www/) are served from
# an overlay of this tree's display.js and gpu-exec.js, and the initramfs is the dist's with this tree's gpu/server/*.js
# appended (an initramfs may be several archives one after the other). --before: the dist as it is, plus only the ballast
# (the tree this one was fixed from). GL_BINS: a directory with the riscv64 es2gears_wayland (tools/gpu-nm.sh's build/gpu-bins).
# CHROME: the test browser (default tools/chrome/...). Runs under tools/with-browser.sh; the page server is a local one
# (tools/gpuaddr-serve.py): nothing is staged on the shared dev server.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
VARIANT=after; FLOOR=4.5; MEM=8192
while [ $# -gt 0 ]; do
	case $1 in
		--before) VARIANT=before; shift ;;
		--floor) FLOOR=$2; shift 2 ;;
		--mem) MEM=$2; shift 2 ;;
		*) echo "usage: $0 [--before] [--floor GIB] [--mem MIB]" >&2; exit 2 ;;
	esac
done
DIST=${DIST:-$ROOT/dist}
GL_BINS=${GL_BINS:-$ROOT/build/gpu-bins}
[ -f "$GL_BINS/es2gears_wayland" ] || { echo "gpuaddr-chrome: no $GL_BINS/es2gears_wayland (GL_BINS)" >&2; exit 2; }
[ -f "$DIST/www/initramfs-wasm64_nommu.cpio.gz" ] || { echo "gpuaddr-chrome: no $DIST/www/initramfs-wasm64_nommu.cpio.gz (DIST)" >&2; exit 2; }
W=$(mktemp -d)
trap 'kill $SERVER 2> /dev/null; rm -rf "$W"' EXIT
mkdir -p $W/stage/usr/lib/rvgpu/bin $W/over
cp gpu/server/ballast.js $W/stage/usr/lib/rvgpu/
cp "$GL_BINS/es2gears_wayland" $W/stage/usr/lib/rvgpu/bin/
chmod 755 $W/stage/usr/lib/rvgpu/bin/*
if [ $VARIANT = after ]; then
	cp gpu/server/boot.js gpu/server/server.js gpu/server/vdec.js $W/stage/usr/lib/rvgpu/
	cp tools/www/display.js tools/www/gpu-exec.js $W/over/
else
	# (the dist's own boot.js has no RVGPU_LOAD: the one line that lets the ballast run in the server's place is added)
	git show "$(git merge-base HEAD "${GPUADDR_BASE:-origin/release}")":gpu/server/boot.js \
		| sed 's|gs.load("server.js");|gs.load(env("RVGPU_LOAD") \|\| "server.js");|' > $W/stage/usr/lib/rvgpu/boot.js
fi
(cd $W/stage && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2> /dev/null | gzip -1 > $W/extra.cpio.gz)
cat "$DIST/www/initramfs-wasm64_nommu.cpio.gz" $W/extra.cpio.gz > $W/over/initramfs-wasm64_nommu.cpio.gz
PORT=$((18700 + RANDOM % 200))
python3 tools/gpuaddr-serve.py $W/over "$DIST/www" "$DIST/disk" $PORT &
SERVER=$!
sleep 1
# (the test browser lives in the main checkout's tools/chrome; a worktree has none of its own)
APP="chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing"
if [ -z "$CHROME" ]; then
	for d in "$ROOT/tools/chrome" ${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/chrome; do [ -x "$d/$APP" ] && CHROME="$d/$APP" && break; done
fi
export CHROME
OUT=${GPUADDR_OUT:-$ROOT/build/gpuaddr-chrome}; mkdir -p $OUT
set +e
tools/with-browser.sh gpuaddr node tools/desktop-drive.js --worker-errors --url http://127.0.0.1:$PORT --disk release --query "mem=$MEM&autostart=0" --sh-timeout 120000 \
	"(debian 'cd /tmp; HOSTNODE_HOME=/run/rvgpu RVGPU_LOAD=ballast.js RVGPU_BALLAST_GIB=$FLOOR /run/hostnode/hostnode > /tmp/ballast.log 2>&1' &)" \
	wait:25 \
	"sh:debian 'tail -3 /tmp/ballast.log; ls /tmp/ballast.*; ps | grep -c hostnode'" \
	"sh:(debian 'RVGPU=1 RVGPU_STATS=1 desktop wayland es2gears_wayland > /tmp/gears.log 2>&1' &)" \
	mark waitwin:es2gears,${GPUADDR_WAIT:-180} wait:20 shot:$OUT/$VARIANT-$FLOOR.png \
	"sh:debian 'tail -3 /tmp/gears.log; grep -a listening /root/rvgpu.log; tail -3 /root/rvgpu.log'" \
	"js:JSON.stringify(window.rvGpu && window.rvGpu.stats)" \
	"sh:debian 'touch /tmp/ballast.verify; sleep 20; grep -a ballast: /tmp/ballast.log | tail -2; ps | grep -v grep | grep -E \"hostnode|es2gears\" | head -5; ls /tmp/runtime-root; dmesg | tail -5'" \
	log:15 2>&1 | tee $OUT/$VARIANT-$FLOOR.txt
rc=${PIPESTATUS[0]}
probe=0
node tools/gears-probe.js $OUT/$VARIANT-$FLOOR.png || probe=1
echo "gpuaddr-chrome $VARIANT floor $FLOOR GiB mem $MEM: drive exit $rc (0: fine, 4: the machine hung, 6: a Worker threw), gears probe exit $probe"
[ $rc = 0 ] && [ $probe = 0 ]
