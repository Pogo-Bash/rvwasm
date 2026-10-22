#!/bin/bash
# (In the VM) VK-07: venus WSI natively. vkr-vtest (vkr on wgvk on Dawn on lavapipe), weston's headless backend,
# and the trixie arm64 chroot's vkcube (Mesa 25.0.7 venus, VN_DEBUG=vtest) presenting to weston with --wsi wayland.
# Two vkcube runs against one vkr-vtest and one weston: first `--c FRAMES` frames (default 100), which must finish;
# then one that keeps drawing while weston-screenshooter takes the compositor's picture, which must show the cube
# (many colours, the LunarG texture's teal). The last line is "ok venus-wsi: ..." or "FAIL venus-wsi: ...".
# weston's headless backend needs its GL renderer (WESTON_RENDERER=gl, llvmpipe) to offer zwp_linux_dmabuf_v1:
# without it (pixman) Mesa's WSI finds no format and vkcube cannot make a swapchain.
# Variables: FRAMES, SHOT_AFTER (seconds before the first screenshot, 3), SHOT_TRIES (25: a second apart),
# CENV (more environment for the client, e.g. WAYLAND_DEBUG=1), CTMO (client timeout), GPU_BUILD (the build).
. "$(dirname "$0")/../../build/env.sh"
LVP_ICD=$W/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
O=$W/out/$NAME-wsi; mkdir -p $O/xdg; chmod 700 $O/xdg; rm -f $O/vt.sock $O/xdg/wl-vk07* $O/xdg/*.png $O/shot*.png $O/*.log
XDG=$O/xdg
# the compositor (headless: no display; screenshots through weston-screenshooter)
XDG_RUNTIME_DIR=$XDG weston --backend=headless --renderer=${WESTON_RENDERER:-gl} --socket=wl-vk07 --width=640 --height=480 \
	--shell=kiosk --debug > $O/weston.log 2>&1 &
WP=$!
for i in $(seq 50); do [ -S $XDG/wl-vk07 ] && break; sleep 0.1; done
# the host: gpuserver's venus half and the page's executor in one process
WGVK_DAWN_DRIVER_FILES=$LVP_ICD VKR_VTEST_STATS=${STATS:-0} VKR_VTEST_EXIT_IDLE=${IDLE:-8} timeout ${TMO:-180} \
	$B/native/src/webgpu/vkr/vkr-vtest --socket $O/vt.sock > $O/server.log 2>&1 &
SP=$!
for i in $(seq 100); do [ -S $O/vt.sock ] && break; sleep 0.1; done
CO=/out/$NAME-wsi
CLIENT="cd $CO; export VN_DEBUG=\${VN_DEBUG:-vtest} VTEST_SOCKET_NAME=$CO/vt.sock VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json XDG_RUNTIME_DIR=$CO/xdg WAYLAND_DISPLAY=wl-vk07 $CENV"
# 1. N frames through the window system: the client must finish
bash $W/chroot-run.sh "$CLIENT; timeout ${CTMO:-60} vkcube --wsi wayland --width 320 --height 240 --c ${FRAMES:-100}" > $O/client-frames.log 2>&1
FR=$?
# 2. a client that keeps drawing while the compositor's screenshot is taken
bash $W/chroot-run.sh "$CLIENT; timeout 60 vkcube --wsi wayland --width 320 --height 240" > $O/client.log 2>&1 &
CP=$!

# a cube is many colours with a teal texture; a black or flat screen is not
cat > $O/eval.py <<'PYEOF'
import zlib, struct, sys
d = open(sys.argv[1], 'rb').read(); i = 8; idat = b''
while i < len(d):
    n, = struct.unpack('>I', d[i:i+4]); t = d[i+4:i+8]; c = d[i+8:i+8+n]
    if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', c[:10])
    if t == b'IDAT': idat += c
    i += 12 + n
raw = zlib.decompress(idat); bpp = {2: 3, 6: 4}[ct]; stride = w * bpp
rows = []; prev = bytearray(stride); p = 0
for y in range(h):
    f = raw[p]; line = bytearray(raw[p+1:p+1+stride]); p += 1 + stride
    for x in range(stride):
        a = line[x-bpp] if x >= bpp else 0; b = prev[x]; c2 = prev[x-bpp] if x >= bpp else 0
        if f == 1: line[x] = (line[x] + a) & 255
        elif f == 2: line[x] = (line[x] + b) & 255
        elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
        elif f == 4:
            pa = abs(b - c2); pb = abs(a - c2); pc = abs(a + b - 2 * c2)
            line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c2)) & 255
    rows.append(line); prev = line
cols = {}
for y in range(0, h, 2):
    for x in range(0, w, 2):
        px = bytes(rows[y][x*bpp:x*bpp+3]); cols[px] = cols.get(px, 0) + 1
top = sorted(cols.items(), key=lambda kv: -kv[1])
bg = top[0][0]; non = sum(n for c, n in cols.items() if c != bg)
teal = sum(n for c, n in cols.items() if c[2] > c[0] + 40 and c[1] > c[0] + 25)
print("screenshot %dx%d: %d distinct colours (sampled), %d samples differ from the most common %s, %d teal (texture)" %
      (w, h, len(cols), non, bg.hex(), teal))
sys.exit(0 if len(cols) > 30 and non > 400 and teal > 100 else 1)
PYEOF

# screenshots until one shows the cube (the client starts slowly on a loaded machine)
sleep ${SHOT_AFTER:-3}
r=1; t=0
for t in $(seq ${SHOT_TRIES:-25}); do
	rm -f $XDG/*.png
	(cd $XDG && XDG_RUNTIME_DIR=$XDG WAYLAND_DISPLAY=wl-vk07 timeout 15 weston-screenshooter > $O/shooter.log 2>&1)
	f=$(ls -t $XDG/*.png 2>/dev/null | head -1)
	if [ -n "$f" ]; then
		cp "$f" $O/shot.png
		python3 $O/eval.py $O/shot.png > $O/eval.log 2>&1; r=$?
		[ $r = 0 ] && break
	fi
	sleep 1
done
kill $CP 2> /dev/null; wait $CP 2> /dev/null
echo "frames client rc $FR (0 = finished); screenshots taken: $t"
kill $WP 2> /dev/null; wait $SP; echo "server rc $?"
echo ==== client; tail -${TAIL:-15} $O/client-frames.log
echo ==== server; tail -${STAIL:-10} $O/server.log
[ -f $O/shot.png ] || { echo "FAIL venus-wsi: no screenshot ($(tail -1 $O/shooter.log))"; exit 1; }
cat $O/eval.log
[ $FR = 0 ] || r=1
if [ $r = 0 ]; then
	echo "ok venus-wsi: vkcube --wsi wayland through venus: ${FRAMES:-100} frames, and the compositor shows the cube"
else
	echo "FAIL venus-wsi: frames client rc $FR, screenshot $(tail -1 $O/eval.log)"
fi
exit $r
