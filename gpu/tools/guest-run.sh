# Run a program on the page's window server and wait for it: run.sh NAME MODE TIMEOUT PROGRAM [ARGS...]
# MODE: drm (rv's render node, RVGPU_DRM=1), vtest (Mesa's virpipe, stage A), soft (RVGPU=0)
name=$1 mode=$2 t=$3; shift 3
export RVGPU=1 RVGPU_STATS=1 WLSERVER_STATS=1 PATH="$PATH:/run/rvgpu/bin:/tmp/g/bin"
case $mode in drm) export RVGPU_DRM=1 ;; vtest) unset RVGPU_DRM ;; soft) export RVGPU=0; unset RVGPU_DRM ;; esac
# (Line-buffered output: the programs are ended by timeout, and a block-buffered stdout went with them: es2gears
# on the render node drew at 17 FPS for 30 s and left an empty log. Not Chromium: desktop knows it by its name.)
case $1 in chromium|*/chromium) ;; *) set -- stdbuf -oL "$@" ;; esac
SECONDS=0
timeout $t desktop wayland "$@" > /root/g-$name.log 2>&1 < /dev/null
echo "== $name ($mode) status $? in ${SECONDS} s: $*"
grep -a -v -E "^rv-virtgpu: (resource|wait|get|put|map)" /root/g-$name.log | tail -${TAILN:-40}
