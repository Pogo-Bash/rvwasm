#!/bin/bash
# vkship-check: the Vulkan ship's browser gates on a site (local dist or the live address), in the test browser, one run each:
#   exec-webgpu   ?exec=webgpu: vulkaninfo --summary shows Venus, vkcube draws, es2gears draws, 0 Worker errors
#   exec-default  no exec parameter: the page's default executor (vrend): es2gears draws, vulkaninfo, 0 Worker errors
# (the desktop with Konsole/Dolphin/Kate/System Settings native is tests/wasmdyn/native-chrome.js on the same address)
#   tools/vkship-check.sh SITE_URL MODE OUTDIR [EXTRA_QUERY]       e.g. http://127.0.0.1:18771  exec-webgpu  build/local-vulkan
# Runs under tools/with-browser.sh. Exit 0: every line of the mode passed.
set -e
cd "$(dirname "$0")/.."
SITE=$1; MODE=$2; OUT=$3; EXTRA=${4:+&$4}
[ -n "$OUT" ] || { sed -n '2,9p' "$0"; exit 2; }
mkdir -p "$OUT"
case $MODE in
	exec-webgpu) Q="exec=webgpu&autostart=0$EXTRA" ;;
	exec-default) Q="autostart=0$EXTRA" ;;
	*) echo "mode: exec-webgpu or exec-default" >&2; exit 2 ;;
esac
APP="chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing"
[ -n "$CHROME" ] || for d in "$PWD/tools/chrome" ${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/chrome; do [ -x "$d/$APP" ] && CHROME="$d/$APP" && break; done
export CHROME
L=$OUT/$MODE
export CHROME_LOG=$L.chrome.log CHROME_FLAGS="--enable-logging=stderr --v=0"
set +e
if [ $MODE = exec-webgpu ]; then
	tools/with-browser.sh vkship node tools/desktop-drive.js --worker-errors --url "$SITE" --disk release --query "$Q" --sh-timeout 240000 \
		"(debian 'desktop wayland vulkaninfo --summary > /tmp/vi.log 2>&1; echo VI_DONE >> /tmp/vi.log' &)" \
		"js:JSON.stringify(window.rvGpu && window.rvGpu.stats && {select: window.rvGpu.stats.select, kind: window.rvGpu.stats.exec_kind, lazy: window.rvGpu.stats.lazy})" \
		"sh:debian 'i=0; while ! grep -q driverName /tmp/vi.log && ! grep -q VI_DONE /tmp/vi.log && [ \$i -lt 60 ]; do sleep 3; i=\$((i+1)); done; sleep 2; grep -E \"deviceName|deviceType|driverName|apiVersion|GPU[0-9]|ERROR|error|Devices\" /tmp/vi.log | head -20; tail -2 /tmp/vi.log; grep -c . /tmp/vi.log; pkill vulkaninfo; echo vi-killed'" \
		"js:JSON.stringify(window.rvGpu && window.rvGpu.stats && {select: window.rvGpu.stats.select, kind: window.rvGpu.stats.exec_kind, lazy: window.rvGpu.stats.lazy, delay: window.rvGpu.stats.lazyDelayMs, fallback: window.rvGpu.stats.fallback})" \
		"sh:(debian 'desktop wayland vkcube --wsi wayland > /tmp/vkcube.log 2>&1' &)" \
		mark waitwin:vkcube,420 wait:25 shot:$L-vkcube.png \
		"sh:debian 'head -5 /tmp/vkcube.log; pkill vkcube; sleep 1; echo killed'" \
		"sh:(debian 'RVGPU_STATS=1 desktop wayland es2gears_wayland > /tmp/gears.log 2>&1' &)" \
		mark waitwin:es2gears,300 wait:25 shot:$L-gears.png \
		"sh:debian 'tail -3 /tmp/gears.log; grep -a -c . /root/rvgpu.log; tail -4 /root/rvgpu.log'" \
		"js:JSON.stringify(window.rvGpu && window.rvGpu.stats && {kind: window.rvGpu.stats.exec_kind, exec: window.rvGpu.stats.exec, ready: window.rvGpu.stats.readyKind, lost: window.rvGpu.stats.deviceLost})" \
		log:25 2>&1 | tee $L.txt
else
	tools/with-browser.sh vkship node tools/desktop-drive.js --worker-errors --url "$SITE" --disk release --query "$Q" --sh-timeout 240000 \
		"(debian 'RVGPU_STATS=1 desktop wayland es2gears_wayland > /tmp/gears.log 2>&1' &)" \
		"js:JSON.stringify(window.rvGpu && window.rvGpu.stats && {select: window.rvGpu.stats.select, kind: window.rvGpu.stats.exec_kind})" \
		mark waitwin:es2gears,420 wait:25 shot:$L-gears.png \
		"sh:debian 'tail -3 /tmp/gears.log; tail -4 /root/rvgpu.log'" \
		"js:JSON.stringify(window.rvGpu && window.rvGpu.stats && {select: window.rvGpu.stats.select, kind: window.rvGpu.stats.exec_kind, exec: window.rvGpu.stats.exec})" \
		"sh:(debian 'desktop wayland vulkaninfo --summary > /tmp/vi.log 2>&1; echo VI_DONE >> /tmp/vi.log' &)" \
		"sh:debian 'i=0; while ! grep -q driverName /tmp/vi.log && ! grep -q VI_DONE /tmp/vi.log && [ \$i -lt 60 ]; do sleep 3; i=\$((i+1)); done; sleep 2; head -14 /tmp/vi.log; pkill vulkaninfo; echo vi-killed'" \
		log:25 2>&1 | tee $L.txt
fi
rc=${PIPESTATUS[0]}
probe=0
node tools/gears-probe.js $L-gears.png || probe=1
echo "vkship-check $MODE: drive exit $rc (0 fine, 4 hung, 6 a Worker threw), gears probe exit $probe"
[ $rc = 0 ] && [ $probe = 0 ]
