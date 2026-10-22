#!/bin/bash
# (In the VM) the captures tests/vfront.test replays: glmark2-es2 scenes through vfront-vtest (this track's
# Mesa-main build, zink on lavapipe) with the guest in the trixie chroot, 0.1 s each at 160x120.
#   make-captures.sh [DIR]      (default ~/rvwasm/gpu-t6/cap)
# glm-small: texture shading effect2d pulsar desktop refract conditionals loop ideas clear (about 16 MB:
# 52 shaders, 1002 draws, 468 readbacks). Compare with virglrenderer: tools/vrend_replay.c (cap-compare.sh).
D=${1:-$HOME/rvwasm/gpu-t6/cap}
mkdir -p $D
S=$(dirname "$0")
args="-s 160x120"
for b in texture:texture-filter=mipmap shading:shading=phong effect2d pulsar:quads=5 desktop:effect=blur:windows=4 \
	refract conditionals:fragment-steps=5:vertex-steps=5 loop:fragment-loop=true ideas clear; do
	args="$args -b $b:duration=0.1"
done
RESTART=0 VFRONT_VTEST_ARGS="--once --capture $D/glm-small.vfcap" bash $S/main-run.sh capg \
	"Xvfb :7 -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & sleep 1; DISPLAY=:7 GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1 timeout 200 glmark2-es2 $args 2>&1 | grep -E '^\[|Score'; kill %1"
ls -la $D/glm-small.vfcap
