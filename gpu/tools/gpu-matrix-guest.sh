#!/bin/bash
# The GPU app matrix, in the machine (VK-17): the apps of docs/gpu-matrix.md run as a person runs them, with a few lines of numbers each.
# tools/gpu-nm.sh runs the Qt Quick, GTK 4 and Chromium cells with this script under node-machine (nm+dawn); the owner runs all of them in
# the page (the manual test sheets, VK-17), once with ?exec=webgpu and once with ?exec=vrend, and compares.
#
#   bash gpu-matrix-guest.sh APP [SECONDS]      APP: es2gears glmark2 qtquick qtquick-vulkan gtk4 chromium mpv vkcube vulkaninfo, or `all`
#
# It lives in /run/rvgpu/matrix (tools/stage.sh and gpu-nm.sh put it there) with spin.qml and anim.html beside it; the programs the disk
# lacks (qml, GTK 4) are in m/ there (gpu/tools/matrix-bundle.sh makes m.tgz in the build VM: `mkdir -p /tmp/matrix; curl -s http://10.0.5.1:8020/m.tgz | tar -C /tmp/matrix -xz`
# when the page's machine does not have them yet). RVGPU=1 is set: a missing executor is an error, not a software run.
# Output per app: the program's own lines, then `MATRIX app=NAME rc=N secs=S`. The frame lines are `N frames in S seconds = F FPS`
# (es2gears' own; the Qt Quick and Chromium pages print the same).
M=$(cd "$(dirname "$0")" && pwd)
# the programs the disk lacks: m/ next to this script (gpu-nm.sh stages it there) or in /tmp/matrix (the owner's `curl ... | tar -C /tmp/matrix -xz`)
MP=$M
[ -x $MP/m/bin/qml ] || MP=/tmp/matrix
export RVGPU=1 RVGPU_STATS=1 WLSERVER_STATS=1 PATH="$PATH:/run/rvgpu/bin"
app=${1:-all}
secs=${2:-}

run() {	# run NAME SECONDS COMMAND...: the command under `desktop wayland`, ended after SECONDS
	local name=$1 t=$2; shift 2
	local t0=$SECONDS
	timeout $t "$@" 2>&1
	local rc=$?
	echo "MATRIX app=$name rc=$rc secs=$((SECONDS - t0))"
}

one() {
	case $1 in
		es2gears)
			run es2gears ${secs:-30} stdbuf -oL desktop wayland es2gears_wayland ;;
		glmark2)
			local d=${GLMARK2_DIR:-$MP/m/glmark2}
			[ -x $d/glmark2-es2-wayland ] || d=/run/rvgpu/glmark2
			run glmark2 ${secs:-600} stdbuf -oL desktop wayland $d/glmark2-es2-wayland --data-path $d/share/glmark2 --off-screen ;;
		qtquick)
			[ -x $MP/m/bin/qml ] || { echo "N/A: no qml in $M/m (gpu/tools/matrix-bundle.sh)"; return 1; }
			QSG_INFO=1 run qtquick ${secs:-90} desktop wayland $MP/m/bin/qml $M/spin.qml ;;
		qtquick-vulkan)
			# (Qt's RHI on Vulkan: venus on the WebGPU executor, lavapipe elsewhere; `desktop` sets QT_VULKAN_LIB, VK-27)
			[ -x $MP/m/bin/qml ] || { echo "N/A: no qml in $M/m (gpu/tools/matrix-bundle.sh)"; return 1; }
			QSG_INFO=1 QSG_RHI_BACKEND=vulkan run qtquick-vulkan ${secs:-90} desktop wayland $MP/m/bin/qml $M/spin.qml ;;
		gtk4)
			[ -x $MP/m/bin/gtk4-widget-factory ] || { echo "N/A: no GTK 4 in $M/m (gpu/tools/matrix-bundle.sh)"; return 1; }
			GSK_RENDERER=${GSK_RENDERER:-ngl} LD_LIBRARY_PATH=$MP/m/lib:$LD_LIBRARY_PATH XDG_DATA_DIRS=$MP/m/share:/usr/local/share:/usr/share \
				run gtk4 ${secs:-90} desktop wayland $MP/m/bin/gtk4-widget-factory ;;
		chromium)
			# (cr-drm: GPU compositing on the render node; a local page, no network)
			RVGPU_DRM=1 run chromium ${secs:-240} desktop wayland chromium --enable-logging=stderr file://$M/anim.html ;;
		mpv)
			local f=${MPV_FILE:-/usr/local/share/rvwasm/test-h264-1080p.mp4}
			RVGPU_DRM=1 run mpv ${secs:-60} desktop wayland mpv --hwdec=vaapi --no-audio --no-input-default-bindings --length=${MPV_LENGTH:-10} $f ;;
		vkcube)
			run vkcube ${secs:-120} stdbuf -oL desktop wayland vkcube --wsi wayland --c 300 ;;
		vulkaninfo)
			run vulkaninfo ${secs:-60} desktop wayland vulkaninfo --summary ;;
		*) echo "gpu-matrix-guest: no app $1" >&2; return 2 ;;
	esac
}

if [ "$app" = all ]; then
	for a in es2gears glmark2 qtquick qtquick-vulkan gtk4 chromium mpv vkcube vulkaninfo; do
		echo "=== $a ($(date +%H:%M:%S))"
		one $a
	done
else
	one $app
fi
