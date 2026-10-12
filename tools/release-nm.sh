#!/bin/bash
# The release under node-machine (tools/node-machine.js: the machine without a browser): the kernel, runtime and
# initramfs the page's server serves (http://localhost:8080/ after tools/release-setup.sh), this tree's window
# server, GPU server and desktop script layered over the initramfs, the release disk fetched from that server,
# and stand-ins for the page: the GPU stub (gpu/tools/gpu-stub.js), the video decoder stub (gpu/tools/vdec-stub.js,
# frames marked drawn into GPU resources) and tools/wl-dump.js, which does what the page does with the window
# server's table (acknowledges frames, so clients get their callbacks and buffers back), takes a person's actions
# (WL_DUMP_ACTIONS: clicks, keys, screenshots; its header says them all) and prints every window's slot as it
# changes. The desktop starts by itself, as in the browser (23 s to the panel natively).
#
#   [WL_DUMP_ACTIONS='...'] [V=1] tools/release-nm.sh 'COMMAND' [TIMEOUT_MS]
#
# COMMAND is typed at the machine's prompt once it is up (the desktop is starting meanwhile); its output and the
# hooks' lines come out here. build/nm-release/ keeps the server's files (NM_FRESH=1: fetch them again after a
# rebuild) and gets the screenshots (shots/). RELEASE_URL: the server (default http://localhost:8080).
#   WL_DUMP_ACTIONS='screen:1280,800,1,0|wait:45|rclick:134,770|wait:4|shot:menu' tools/release-nm.sh 'sleep 60' 120000
set -e
cd "$(dirname "$0")/.."
URL=${RELEASE_URL:-http://localhost:8080}
N=build/nm-release
mkdir -p $N/rt $N/shots
for f in linux.js linux-worker.js vmlinux-wasm64_nommu.wasm initramfs-wasm64_nommu.cpio.gz; do
	case $f in linux*.js) to=$N/rt/$f ;; *) to=$N/$f ;; esac
	if [ -n "$NM_FRESH" ] || [ ! -s "$to" ]; then
		curl -sf -m 120 -o "$to" "$URL/$f" || { echo "release-nm: cannot fetch $URL/$f (is the release served? tools/release-setup.sh)" >&2; exit 1; }
	fi
done
rm -rf $N/stage
mkdir -p $N/stage/usr/lib/wlserver $N/stage/usr/lib/rvgpu $N/stage/bin
cp -r wlserver/. $N/stage/usr/lib/wlserver/
cp tools/x/desktop $N/stage/usr/lib/wlserver/desktop
# NM_ENV='WLSERVER_DEBUG=1 WLSERVER_STATS=1': in the desktop's environment (the autostart's has nothing of this one)
[ -z "$NM_ENV" ] || sed -i.bak "2i\\
export $NM_ENV
" $N/stage/usr/lib/wlserver/desktop && rm -f $N/stage/usr/lib/wlserver/desktop.bak
cp gpu/server/*.js $N/stage/usr/lib/rvgpu/
cp tools/debian-enter $N/stage/bin/debian
chmod 755 $N/stage/bin/debian $N/stage/usr/lib/wlserver/desktop
(cd $N/stage && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2> /dev/null | gzip -1 > ../extra.cpio.gz)
opts=(--disk release --server "$URL" --hook "${HOOK:-gpu/tools/gpu-stub.js}")
[ -n "$V" ] && opts+=(--verbose)
export GPU_STUB_CHAIN=${GPU_STUB_CHAIN:-gpu/tools/vdec-stub.js} VDEC_STUB_CHAIN=${VDEC_STUB_CHAIN:-tools/wl-dump.js} VDEC_STUB_GPU=${VDEC_STUB_GPU:-1}
export WL_DUMP_DIR=${WL_DUMP_DIR:-$N/shots} WL_DUMP_MAX=${WL_DUMP_MAX:-0}
exec node --experimental-wasm-memory64 tools/node-machine.js --runtime $N/rt --kernel $N/vmlinux-wasm64_nommu.wasm \
	--initramfs $N/initramfs-wasm64_nommu.cpio.gz --initramfs $N/extra.cpio.gz --timeout "${2:-180000}" "${opts[@]}" "$1"
