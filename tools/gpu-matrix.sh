#!/bin/bash
# gpu-matrix: the GPU app matrix, apps x executors (VK-17; gates the default switch from vrend to webgpu, plan R3).
# Every app runs through tools/gpu-nm.sh ("nm+dawn": node-machine with the REAL rvgpu executor on Dawn's Node bindings), and the
# results are printed as one markdown table (the one in docs/gpu-matrix.md).
#
#   tools/gpu-matrix.sh [OPTIONS] [APP ...]
#
# APPS (default: all of them, in this order)
#   es2gears glmark2 qtquick qtquick-vulkan chromium gtk4 mpv vkcube vulkaninfo
#
# OPTIONS
#   --out FILE      write the markdown table (default: print it; docs/gpu-matrix.md has it with the notes around it)
#   --table-only    do not run anything: make the table from the last runs in build/gpu-matrix
#   --dir DIR       where the runs go (default build/gpu-matrix): webgpu-APP.txt (gpu-nm's report), .rc, .wall; stub-APP.txt
#   --no-stub       skip the stub runs (below)
#   --stub-only     only the stub runs (below)
#   --secs N        how long a program that does not end by itself runs (gpu-nm -t; default: the program's own)
#   ...             any other option goes to tools/gpu-nm.sh (--keep, -v, --fetch-rvgpu BUILD, -T MS)
#
# Columns. webgpu: the app through gpu-nm.sh on the real executor (a PASS is gpu-nm's own verdict: the program's checks, a moved ring,
# a drawn window). vrend: vrend is the page's WebGL renderer (tools/www/gpu-exec.js), there is none under Node, so the GL apps' cells
# are `n/a` and the owner fills them with the same apps in the browser (the manual test sheets, VK-17; the apps' commands are
# in gpu/tools/gpu-matrix-guest.sh, which runs in the page's machine). The Vulkan apps do have a vrend-side answer under Node: an
# executor with no Vulkan (`gpu-nm.sh --stub`, gpu/tools/gpu-stub.js), which is what the GPU server says when vrend is the executor;
# vulkaninfo and vkcube run that way as well and fall to lavapipe.
#
# Memory: every run is a node-machine under ${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/nm-slot.sh (it waits for a slot).
set -e
cd "$(dirname "$0")/.."
ALL="es2gears glmark2 qtquick qtquick-vulkan chromium gtk4 mpv vkcube vulkaninfo"
DIR=build/gpu-matrix; OUT=; TABLE_ONLY=; STUBS=1; STUB_ONLY=; SECS=; PASS=()
APPS=()
while [ $# -gt 0 ]; do
	case $1 in
		--out) OUT=$2; shift 2 ;;
		--table-only) TABLE_ONLY=1; shift ;;
		--dir) DIR=$2; shift 2 ;;
		--no-stub) STUBS=; shift ;;
		--stub-only) STUB_ONLY=1; shift ;;
		--secs) SECS=$2; shift 2 ;;
		-h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d'; exit 0 ;;
		-T|--fetch-rvgpu) PASS+=("$1" "$2"); shift 2 ;;
		-*) PASS+=("$1"); shift ;;
		*) APPS+=("$1"); shift ;;
	esac
done
[ ${#APPS[@]} -gt 0 ] || APPS=($ALL)
NODE=gpu/webgpu/tests/dawn-node/node_modules/.bin/node
[ -x $NODE ] || NODE=node
mkdir -p "$DIR"

# per-app options: how long each runs (seconds; gpu-nm's defaults are 45 for es2gears, glmark2 all scenes, vkcube 300 frames)
opts_for() {
	case $1 in
		es2gears) echo "-t ${SECS:-30}" ;;
		qtquick|qtquick-vulkan) echo "-t ${SECS:-30}" ;;
		chromium) echo "-t ${SECS:-120}" ;;
		gtk4) echo "-t ${SECS:-30}" ;;
		vkcube) echo "-t ${SECS:-120}" ;;
		mpv) ;;
		*) [ -z "$SECS" ] || echo "-t $SECS" ;;
	esac
}

run_one() {	# run_one PREFIX APP [gpu-nm options]
	local prefix=$1 app=$2; shift 2
	local f="$DIR/$prefix-$app"
	local t0=$(date +%s)
	echo "gpu-matrix: $prefix $app ($(date +%H:%M:%S))" >&2
	set +e
	tools/gpu-nm.sh "$@" $(opts_for $app) "${PASS[@]}" $app > "$f.txt" 2> "$f.err"
	echo $? > "$f.rc"
	set -e
	echo $(( $(date +%s) - t0 )) > "$f.wall"
	tail -n 3 "$f.txt" | cut -c1-200 >&2
}

if [ -z "$TABLE_ONLY" ]; then
	node -e 'console.log(JSON.stringify({commit: process.argv[1], branch: process.argv[2], date: new Date().toISOString().slice(0, 16).replace("T", " "), node: process.version}))' \
		"$(git rev-parse --short HEAD)" "$(git rev-parse --abbrev-ref HEAD)" > "$DIR/meta.json"
	for app in "${APPS[@]}"; do
		[ -n "$STUB_ONLY" ] || run_one webgpu $app
		if [ -n "$STUBS" ]; then
			case $app in vulkaninfo|vkcube) run_one stub $app --stub ;; esac
		fi
	done
fi
if [ -n "$OUT" ]; then
	$NODE gpu/tools/gpu-matrix-table.js "$DIR" > "$OUT"
	echo "gpu-matrix: table in $OUT" >&2
else
	$NODE gpu/tools/gpu-matrix-table.js "$DIR"
fi
