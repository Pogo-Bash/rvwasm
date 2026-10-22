#!/bin/bash
# gpu-nm: "nm+dawn", a GPU program on the machine under node-machine with the REAL executor: the page's
# rvgpu-worker.js (rvgpu.wasm: vfront on zink on wgvk, and vkr) in a Node Worker on Dawn's Node bindings (the Mac's
# Metal), instead of the stub that only answers gpuserver with a colour. No browser. VK-06.
#
#   tools/gpu-nm.sh [OPTIONS] PROGRAM [ARGS...]
#
# PROGRAM (run in the Debian system of the machine, on the window server, RVGPU=1: it fails when no executor answers):
#   es2gears          es2gears_wayland for SECONDS (-t, default 45): stage A (Mesa's readback into wl_shm). Prints the
#                     program's frames line, the window server's presents, the ring counters and a PIXEL PROBE of the
#                     window (red, green and blue gear colours present; the clear colour still the background)
#   eglinfo           eglinfo (the GL the guest gets from the executor's caps)
#   glmark2 [ARGS]    glmark2-es2-wayland --off-screen (default) or ARGS, in the machine (needs the program: see below)
#   vkr-bench [N]     vkr-bench (T5's venus micro-benchmark) through venus: every fence comes back and every round reads the right word
#   vulkaninfo        `desktop wayland vulkaninfo --summary` as a person types it, timed (no setup script): venus on the
#                     real executor, lavapipe beside it, or only lavapipe when the executor has no Vulkan (--stub)
#   venus-fail        (with --stub) the venus driver alone against an executor without Vulkan: it must fail at once, not hang (VK-08a)
#   vkcube            vkcube --wsi wayland through venus (stage B: the executor's presents are in the summary / PNGs)
#   mpv [CLIP]        mpv --hwdec=vaapi on a short H.264 clip (default h264-high.mp4, the build VM's ~/vdec/media), the page's video path
#                     with the real executor (VK-14): the guest's VA-API driver sends the chunks to gpuserver, a stand-in for the
#                     page's WebCodecs decoder (gpu/tools/vdec-stub.js, VDEC_STUB_GPU=executor) answers with a TEST CARD (colour bars,
#                     grey ramp, a moving marker) as frames, and rvgpu-worker.js draws them into the guest's surfaces
#                     (rvGpu.stats.exec.vdecDrawn); mpv shows them and the window's PNG is probed for the bars. -t is mpv's --length
#                     (default 2 s of the clip). --exact: the exact path (copyTo + writeTexture) instead of the shader.
#   qtquick           a Qt Quick scene (gpu/tools/matrix/spin.qml: 60 spinning rectangles + text) with `qml`, through GL on the executor
#                     (VK-17; gpu/tools/gpu-matrix-guest.sh runs it). Prints the scene's own `N frames in S seconds = F FPS`. -t is how long (default 90)
#   qtquick-vulkan    the same scene on Qt's RHI Vulkan backend (QSG_RHI_BACKEND=vulkan): venus on the executor (VK-27)
#   gtk4              gtk4-widget-factory (GTK 4.18, GSK_RENDERER=ngl: GL ES 3 through the executor); -t default 90 (VK-17)
#   chromium          Chromium with GPU compositing on the render node (RVGPU_DRM=1, ANGLE on Mesa's EGL: the "cr-drm" of the manual test sheets)
#                     on gpu/tools/matrix/anim.html (120 CSS boxes + a canvas; prints its frame rate to the console); -t default 240 (VK-17)
#   vkcube-trace [S]  vkcube for SECONDS (-t), and after S seconds (default 30) gpu/tools/stageb-trace.py in the machine: the window server's
#                     table and the GPU server's ring, present by present (commit, GPU done, shown: VK-09)
#   blender-bg        Blender 4.3 (the disk's riscv64 package, GPU_NM_DISK with blender: see below) renders the default scene with
#                     Workbench, 480x270, through `desktop wayland blender -b` (virpipe -> vfront on the real executor); the PNG comes
#                     back as base64 and is compared with the Workbench look (VK-16)
#   blender           the Blender window for SECONDS (-t, default 120): `desktop wayland blender --factory-startup` on the window
#                     server; the window's PNGs (shots/) are probed for the UI and the viewport's cube (VK-16)
#   sh:COMMAND        COMMAND in the machine's shell as it is (no RVGPU, no window server started for you)
#   dbg:COMMAND       COMMAND in the Debian system, RVGPU=1, as `desktop wayland` would run a program: dbg:'desktop wayland X'
#
# OPTIONS
#   -t SECONDS        how long a program that does not end by itself runs (default 45)
#   -T MS             node-machine's timeout for the whole run (default 300000)
#   -v                everything the machine and the executor say (stderr), as it comes
#   --fetch-rvgpu B   copy rvgpu.{js,wasm} of the build VM's build B (~/rvwasm/webgpu/b/B/wasm/src/webgpu/rvgpu) into the
#                     executor's directory first (RVGPU_DIR, default build/rvgpu-page). gpu/webgpu/build/vm.sh
#                     gpu/webgpu/build/wasm.sh builds it; the executor refuses an unstamped or dirty one (tools/rvgpu-gate.sh)
#   --no-fresh-rv     the machine's /bin/rv is this tree's build when build/vdec-nm/rv exists (the stage initramfs's is old: no PRIME import); this keeps the stage's
#   --staged          the machine is ONLY what the dev server hands out (initramfs, kernel, runtime): nothing of this tree goes on top of it (no window
#                     server, GPU server, desktop script, programs, rv); the executor is RVGPU_DIR's. For tools/../gpu/tools/vulkan-setup.sh check:
#                     the staged build as the owner's browser gets it. Implies --no-fresh-rv
#   --exact           (mpv) VDEC_STUB_EXACT=1: frames drawn by copying the planes (the page's ?vdec=exact)
#   --stub            the old way: the stub executor (gpu/tools/gpu-stub.js), for comparison; Node 22 is enough
#   --keep            leave build/gpu-nm/ as it is (no new extra.cpio.gz)
#   --wait            (kept for old callers: the run always waits for a slot, see below)
#
# ENVIRONMENT
#   RVGPU_DIR  rvgpu-worker.js + rvgpu/ (default build/rvgpu-page); RVGPU_NM_*, DAWN_OPTS: gpu/tools/rvgpu-nm.js's
#   GPU_NM_URL   the dev server that has the kernel, runtime, initramfs and the disk (default http://localhost:8000)
#   GPU_NM_DISK  the disk (default release); GPU_NM_FRESH=1 fetch the kernel/runtime/initramfs again
#   GPU_NM_DIR   where it all goes (default build/gpu-nm); the window PNGs in $GPU_NM_DIR/shots
#   WL_DUMP_ACTIONS  tools/wl-dump.js's actions (clicks, keys, screenshots) in the same run
#   NM_ENV           'A=1 B=2': in the desktop's environment (WLSERVER_DEBUG=1 ...)
#   GPU_NM_APPEND    more words for the kernel's command line (mem=8192M: the page's default on a big computer)
#   GPU_NM_PRE/POST  (es2gears, vkcube, mpv) shell commands in the Debian system before / after the program (no single quotes in them):
#                    gpu/tools/gpuaddr-check.sh starts a memory ballast with the first and reads what it says with the second
#
# The Node it needs is 26 (npm package `node` in gpu/webgpu/tests/dawn-node; `npm ci` there once: this script does it):
# rvgpu.wasm uses JSPI, node-machine.js runs on it as well. The machine runs under ${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/nm-slot.sh (at most two node-machines on this Mac, only with more than 40% memory free; it waits for a slot). The riscv64 programs come from the build VM the first time and stay
# in build/: mesa-utils (~/rvwasm/gpuguest/x -> build/gpu-bins, /run/rvgpu/bin in the machine), Debian's venus driver,
# vulkaninfo, vkcube and T5's vkr-bench (-> build/gpu-venus, /run/rvgpu/venus), glmark2-es2-wayland and its data
# (~/gpuwork/deb -> build/gpu-glmark2, /run/rvgpu/glmark2).
#
# Exit status: 0 when the program's own checks pass (frames > 0 and, for es2gears, the probe), else 1; 2: set-up.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
URL=${GPU_NM_URL:-http://localhost:8000}
N=${GPU_NM_DIR:-build/gpu-nm}
DISK=${GPU_NM_DISK:-release}
SECS=45; TIMEOUT=300000; VERBOSE=; FETCH=; STUB=; KEEP=; WAIT=; EXACT=; NOFRESHRV=; STAGED=
while [ $# -gt 0 ]; do
	case $1 in
		-t) SECS=$2; SECS_SET=1; shift 2 ;;
		-T) TIMEOUT=$2; TIMEOUT_SET=1; shift 2 ;;
		-v) VERBOSE=1; shift ;;
		--fetch-rvgpu) FETCH=$2; shift 2 ;;
		--stub) STUB=1; shift ;;
		--keep) KEEP=1; shift ;;
		--exact) EXACT=1; shift ;;
		--wait) WAIT=1; shift ;;
		--no-fresh-rv) NOFRESHRV=1; shift ;;
		--staged) STAGED=1; NOFRESHRV=1; shift ;;
		-h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d'; exit 0 ;;
		*) break ;;
	esac
done
[ $# -ge 1 ] || { sed -n '2,/^set -e/p' "$0" | sed '$d' | head -20; exit 2; }
PROG=$1; shift

# --- machines at a time: the coordinator's nm-slot.sh (2026-10-13) holds one of the two machine-wide slots (atomic, waits for >40% free
# memory, frees a dead holder's); this script runs itself under it. NM_SLOT_HELD=1: already inside one (or NM_SLOT=off to skip it).
NMSLOT=${NM_SLOT_SCRIPT:-${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/nm-slot.sh}
if [ -z "$NM_SLOT_HELD" ] && [ "$NM_SLOT" != off ]; then
	[ -x "$NMSLOT" ] || { echo "gpu-nm: no $NMSLOT (the machine-wide slot script)" >&2; exit 2; }
	export NM_SLOT_HELD=1
	exec "$NMSLOT" "$ROOT/tools/gpu-nm.sh" ${WAIT:+--wait} ${VERBOSE:+-v} ${KEEP:+--keep} ${STUB:+--stub} ${EXACT:+--exact} ${STAGED:+--staged} ${NOFRESHRV:+--no-fresh-rv} ${FETCH:+--fetch-rvgpu "$FETCH"} ${SECS_SET:+-t "$SECS"} ${TIMEOUT_SET:+-T "$TIMEOUT"} "$PROG" "$@"
fi

# --- Node 26
DN=gpu/webgpu/tests/dawn-node
if [ -z "$STUB" ]; then
	[ -x $DN/node_modules/.bin/node ] || (cd $DN && npm ci > /dev/null 2>&1) || { echo "gpu-nm: npm ci in $DN failed" >&2; exit 2; }
	NODE=$ROOT/$DN/node_modules/.bin/node
	NODE_FLAGS=
	HOOK=gpu/tools/rvgpu-nm.js
else
	NODE=node
	NODE_FLAGS=--experimental-wasm-memory64
	HOOK=gpu/tools/gpu-stub.js
fi

# --- the executor
RVGPU_DIR=${RVGPU_DIR:-$ROOT/build/rvgpu-page}
export RVGPU_DIR
if [ -n "$FETCH" ]; then
	mkdir -p "$RVGPU_DIR/rvgpu"
	for f in rvgpu.js rvgpu.wasm; do
		scp -q -F "$HOME/.lima/rvwasm/ssh.config" lima-rvwasm:rvwasm/webgpu/b/$FETCH/wasm/src/webgpu/rvgpu/$f "$RVGPU_DIR/rvgpu/$f"
	done
fi
if [ -z "$STUB" ]; then
	[ -f "$RVGPU_DIR/rvgpu/rvgpu.wasm" ] || { echo "gpu-nm: no $RVGPU_DIR/rvgpu/rvgpu.wasm (--fetch-rvgpu BUILD)" >&2; exit 2; }
	tools/rvgpu-gate.sh "$RVGPU_DIR/rvgpu/rvgpu.wasm" || { echo "gpu-nm: that rvgpu.wasm is not a stamped, clean build (the executor refuses it)" >&2; exit 2; }
fi

# --- the machine: kernel, runtime, initramfs from the dev server; this tree's window server, GPU server and desktop on top
mkdir -p $N/rt $N/shots
for f in linux.js linux-worker.js vmlinux-wasm64_nommu.wasm initramfs-wasm64_nommu.cpio.gz; do
	case $f in linux*.js) to=$N/rt/$f ;; *) to=$N/$f ;; esac
	if [ -n "$GPU_NM_FRESH" ] || [ ! -s "$to" ]; then
		curl -sf -m 120 -o "$to" "$URL/$f" || { echo "gpu-nm: cannot fetch $URL/$f (the dev server: $URL)" >&2; exit 2; }
	fi
done
# riscv64 programs of the build VM (mesa-utils-bin, vulkan-tools), once
mkdir -p build/gpu-bins
if [ -z "$STAGED" ] && { [ ! -e build/gpu-venus/libvulkan_virtio.so ] || [ ! -e build/gpu-venus/libvulkan_lvp.so ]; }; then
	mkdir -p build/gpu-venus
	ssh -F "$HOME/.lima/rvwasm/ssh.config" lima-rvwasm 'set -e; T=$(mktemp -d); cd $T; dpkg -x ~/rvwasm/gpuguest/mesa-vulkan-drivers_*_riscv64.deb root; dpkg -x ~/rvwasm/gpuguest/vulkan-tools_*_riscv64.deb root; mkdir out; cp root/usr/lib/riscv64-linux-gnu/libvulkan_virtio.so root/usr/lib/riscv64-linux-gnu/libvulkan_lvp.so root/usr/bin/vulkaninfo root/usr/bin/vkcube out/; cp ~/rvwasm/webgpu/out/t5/guest/vkr-bench-rv64 ~/rvwasm/webgpu/out/t5/guest/add.spv out/; tar cf - -C out .; rm -rf $T' | tar xf - -C build/gpu-venus
fi
if [ "$PROG" = glmark2 ] && [ ! -e build/gpu-glmark2/glmark2-es2-wayland ]; then
	# (the riscv64 debs the GPU work fetched into the build VM before: ~/gpuwork/deb)
	mkdir -p build/gpu-glmark2
	ssh -F "$HOME/.lima/rvwasm/ssh.config" lima-rvwasm 'set -e; T=$(mktemp -d); cd $T; for d in ~/gpuwork/deb/glmark2-es2-wayland_*_riscv64.deb ~/gpuwork/deb/glmark2-data_*_all.deb; do dpkg -x $d x; done; mkdir -p out/share; cp x/usr/bin/glmark2-es2-wayland out/; cp -r x/usr/share/glmark2 out/share/; tar cf - -C out .; rm -rf $T' | tar xf - -C build/gpu-glmark2
fi
# the video bundle (mpv, ffmpeg, libraries the disk lacks, the VA-API driver: gpu/tools/vdec-bundle.sh made it in the build VM) and a clip, once
if [ "$PROG" = mpv ]; then
	CLIP=${1:-h264-high.mp4}
	mkdir -p build/vdec-nm/media
	if [ ! -e build/vdec-nm/v/bin/mpv ]; then
		scp -q -F "$HOME/.lima/rvwasm/ssh.config" lima-rvwasm:vdec/www/v.tgz build/vdec-nm/v.tgz && tar xzf build/vdec-nm/v.tgz -C build/vdec-nm || { echo "gpu-nm: cannot fetch the video bundle ~/vdec/www/v.tgz from the build VM (gpu/tools/vdec-bundle.sh makes it)" >&2; exit 2; }
	fi
	[ -s build/vdec-nm/media/$CLIP ] || scp -q -F "$HOME/.lima/rvwasm/ssh.config" lima-rvwasm:vdec/media/$CLIP build/vdec-nm/media/$CLIP || { echo "gpu-nm: no clip $CLIP in the build VM's ~/vdec/media" >&2; exit 2; }
fi
# Qt Quick's `qml` and GTK 4, which the disk lacks (gpu/tools/matrix-bundle.sh made m.tgz in the build VM), once
if [ "$PROG" = qtquick ] || [ "$PROG" = qtquick-vulkan ] || [ "$PROG" = gtk4 ]; then
	if [ ! -x build/gpu-matrix-pkgs/m/bin/qml ]; then
		mkdir -p build/gpu-matrix-pkgs
		scp -q -F "$HOME/.lima/rvwasm/ssh.config" lima-rvwasm:vk17/www/m.tgz build/gpu-matrix-pkgs/m.tgz && tar xzf build/gpu-matrix-pkgs/m.tgz -C build/gpu-matrix-pkgs || { echo "gpu-nm: cannot fetch ~/vk17/www/m.tgz from the build VM (gpu/tools/matrix-bundle.sh makes it)" >&2; exit 2; }
	fi
fi
if [ -z "$STAGED" ] && [ ! -e build/gpu-bins/es2gears_wayland ]; then
	ssh -F "$HOME/.lima/rvwasm/ssh.config" lima-rvwasm 'cd ~/rvwasm/gpuguest/x/usr/bin; tar cf - es2gears_wayland.riscv64-linux-gnu eglgears_wayland.riscv64-linux-gnu eglinfo.riscv64-linux-gnu es2_info.riscv64-linux-gnu peglgears.riscv64-linux-gnu vkcube vulkaninfo vkgears.riscv64-linux-gnu' | tar xf - -C build/gpu-bins
	for f in build/gpu-bins/*.riscv64-linux-gnu; do mv "$f" "${f%.riscv64-linux-gnu}"; done
fi
if [ -z "$KEEP" ] || [ ! -s $N/extra.cpio.gz ]; then
	rm -rf $N/stage
	mkdir -p $N/stage/usr/lib/wlserver $N/stage/usr/lib/rvgpu/bin $N/stage/bin
	if [ -z "$STAGED" ]; then
		cp -r wlserver/. $N/stage/usr/lib/wlserver/
		cp tools/x/desktop $N/stage/usr/lib/wlserver/desktop
		[ -z "$NM_ENV" ] || sed -i.bak "2i\\
	export $NM_ENV
	" $N/stage/usr/lib/wlserver/desktop && rm -f $N/stage/usr/lib/wlserver/desktop.bak
		cp gpu/server/*.js $N/stage/usr/lib/rvgpu/
		cp build/gpu-bins/* $N/stage/usr/lib/rvgpu/bin/
		cp gpu/tools/stageb-trace.py $N/stage/usr/lib/rvgpu/bin/
		# Vulkan through venus, as the page's initramfs has it (tools/stage.sh): /run/rvgpu/venus with Debian's riscv64
		# libvulkan_virtio.so and libvulkan_lvp.so, vulkaninfo, vkcube, T5's vkr-bench and its shader, and the ICD files;
		# `desktop wayland` points the loader at them (tools/x/desktop, vulkan_use). No setup script, no env file.
		gpu/tools/venus-stage.sh $N/stage/usr/lib/rvgpu/venus build/gpu-venus/*
		if [ "$PROG" = mpv ]; then
			mkdir -p $N/stage/usr/lib/rvgpu/va $N/stage/usr/lib/rvgpu/media
			cp -r build/vdec-nm/v $N/stage/usr/lib/rvgpu/vdec
			cp build/vdec-nm/v/va/rvwasm_drv_video.so $N/stage/usr/lib/rvgpu/va/
			cp build/vdec-nm/media/$CLIP $N/stage/usr/lib/rvgpu/media/
			# (rv of this tree, wasm, if build/vdec-nm/rv is there: the machine's rv must import a buffer across opens of the render node (the player's
			# EGL import of the driver's surfaces); a dev server whose initramfs is older says 'PRIME import ... not done yet'. Build: in the build VM,
			# make -C rv O=~/rvwasm/build-rv-wasm-NAME CC=tools/wasm-cc AR=llvm-ar, as tools/stage.sh does)
			[ ! -s build/vdec-nm/rv ] || { cp build/vdec-nm/rv $N/stage/bin/rv; chmod 755 $N/stage/bin/rv; }
		fi
		# the matrix's guest script and its pages (VK-17), and qml / GTK 4 for the two apps that need them
		mkdir -p $N/stage/usr/lib/rvgpu/matrix
		cp gpu/tools/gpu-matrix-guest.sh gpu/tools/matrix/spin.qml gpu/tools/matrix/anim.html $N/stage/usr/lib/rvgpu/matrix/
		if [ "$PROG" = qtquick ] || [ "$PROG" = qtquick-vulkan ] || [ "$PROG" = gtk4 ] || { [ "${PROG#dbg:}" != "$PROG" ] && [ -x build/gpu-matrix-pkgs/m/bin/qml ]; }; then
			cp -r build/gpu-matrix-pkgs/m $N/stage/usr/lib/rvgpu/matrix/m
		fi
		if [ -z "$NOFRESHRV" ] && [ -s build/vdec-nm/rv ]; then
			cp build/vdec-nm/rv $N/stage/bin/rv; chmod 755 $N/stage/bin/rv
		fi
		if [ "$PROG" = glmark2 ]; then
			mkdir -p $N/stage/usr/lib/rvgpu/glmark2
			cp -r build/gpu-glmark2/. $N/stage/usr/lib/rvgpu/glmark2/
		fi
		[ ! -d build/gpu-share ] || cp -r build/gpu-share $N/stage/usr/lib/rvgpu/share
	fi
	# (blender-render SECONDS: Blender's Workbench render of the default scene, 480x270, in the background through
	# `desktop wayland`; Blender does not end after "Blender quit" in this machine (with the GPU or without), so it is
	# killed once it has said so, or after SECONDS; the PNG goes out as base64 lines for gpu-nm-report.js. VK-16)
	cat > $N/stage/usr/lib/rvgpu/bin/blender-render <<'BREOF'
#!/bin/sh
export HOME=/tmp/blhome
mkdir -p $HOME; cd $HOME; rm -f r_*.png bl.log
desktop wayland blender -b --factory-startup -E BLENDER_WORKBENCH --python-expr "import bpy; s = bpy.context.scene; s.render.resolution_x = 480; s.render.resolution_y = 270; s.render.resolution_percentage = 100" -o $HOME/r_ -F PNG -f 1 > bl.log 2>&1 &
bp=$!
i=0
while [ $i -lt ${1:-600} ] && ! grep -q "Blender quit" bl.log 2> /dev/null && kill -0 $bp 2> /dev/null; do
	i=$((i + 1)); sleep 1
done
sleep 1
pkill -9 blender 2> /dev/null; kill -9 $bp 2> /dev/null
cat bl.log
echo "RENDER_WAITED=$i s"
base64 $HOME/r_0001.png | sed "s/^/PNG64 /"
BREOF
	# (vk-time COMMAND: COMMAND and how long it ran, for the vulkaninfo check)
	printf '#!/bin/sh\ns=$(date +%%s%%N); "$@"; rc=$?; e=$(date +%%s%%N)\necho "VK_TIME_MS=$(( (e - s) / 1000000 )) rc=$rc: $*"\nexit $rc\n' > $N/stage/usr/lib/rvgpu/bin/vk-time
	[ -n "$STAGED" ] || cp tools/debian-enter $N/stage/bin/debian
	chmod 755 $N/stage/bin/debian $N/stage/usr/lib/wlserver/desktop $N/stage/usr/lib/rvgpu/bin/* 2> /dev/null || true
	(cd $N/stage && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2> /dev/null | gzip -1 > ../extra.cpio.gz)
fi

# --- what to run
# (glmark2 runs all its scenes: minutes on this machine)
[ "$PROG" != glmark2 ] || { [ -n "$SECS_SET" ] || SECS=600; [ -n "$TIMEOUT_SET" ] || TIMEOUT=900000; }
G="RVGPU=1 RVGPU_STATS=1 WLSERVER_STATS=1 PATH=\$PATH:/run/rvgpu/bin"
case $PROG in
	es2gears)  CMD="debian 'export $G; $GPU_NM_PRE timeout $SECS stdbuf -oL desktop wayland es2gears_wayland 2>&1; echo PROGRAM_RC=\$?; $GPU_NM_POST'" ;;
	eglinfo)   CMD="debian 'export $G; desktop wayland eglinfo 2>&1 | head -60; echo PROGRAM_RC=\$?'" ;;
	glmark2)   A=${*:---off-screen}; CMD="debian 'export $G; timeout $SECS stdbuf -oL desktop wayland /run/rvgpu/glmark2/glmark2-es2-wayland --data-path /run/rvgpu/glmark2/share/glmark2 $A 2>&1; echo PROGRAM_RC=\$?'" ;;
	vkr-bench) CMD="debian 'export $G; timeout $SECS desktop wayland vkr-bench-rv64 /run/rvgpu/venus/add.spv ${1:-1000} 3 2>&1; echo PROGRAM_RC=\$?'" ;;
	vulkaninfo) CMD="debian 'export $G; timeout $SECS desktop wayland vk-time vulkaninfo --summary 2>&1; echo PROGRAM_RC=\$?'" ;;
	venus-fail) CMD="debian 'export $G; VN_DEBUG=vtest VTEST_SOCKET_NAME=/tmp/runtime-root/rvgpu-0 VK_DRIVER_FILES=/run/rvgpu/venus/virtio_icd.json timeout $SECS desktop wayland vk-time vulkaninfo --summary 2>&1; echo PROGRAM_RC=\$?'" ;;
	mpv)       [ -n "$SECS_SET" ] || SECS=2
	           CMD="debian 'export $G RVGPU_DRM=1 $MPV_ENV; $GPU_NM_PRE TAILN=${MPV_TAIL:-60} bash /run/rvgpu/vdec/run.sh mpv ${MPV_TIMEOUT:-150} mpv --msg-level=all=v --hwdec=vaapi --no-audio --no-input-default-bindings --length=$SECS /run/rvgpu/media/$CLIP 2>&1; grep -a -E \"hwdec|vaapi|VA-API|libva|[Ee]rror|[Ff]ailed|EGL|gpu/|rvvdec|virtgpu\" /root/v-mpv.log | grep -a -v \"^V:\" | tail -${MPV_LOG:-40}; echo PROGRAM_RC=\$?; $GPU_NM_POST'"
	           [ -n "$TIMEOUT_SET" ] || TIMEOUT=900000
	           export RVGPU_NM_CHAIN=gpu/tools/vdec-stub.js VDEC_STUB_GPU=executor VDEC_STUB_CHAIN=tools/wl-dump.js RVGPU_NM_SHOTS=${RVGPU_NM_SHOTS:-24}
	           [ -z "$EXACT" ] || export VDEC_STUB_EXACT=1 ;;
	vkcube)    CMD="debian 'export $G; $GPU_NM_PRE timeout $SECS stdbuf -oL desktop wayland vkcube --wsi wayland --c ${1:-300} 2>&1; echo PROGRAM_RC=\$?; $GPU_NM_POST'" ;;
	blender-bg) [ -n "$SECS_SET" ] || SECS=600; [ -n "$TIMEOUT_SET" ] || TIMEOUT=$((SECS * 1000 + 600000))
	           CMD="debian 'export $G; blender-render $SECS 2>&1; echo PROGRAM_RC=0'" ;;
	blender)   [ -n "$SECS_SET" ] || SECS=120; [ -n "$TIMEOUT_SET" ] || TIMEOUT=$((SECS * 1000 + 600000))
	           CMD="debian 'export $G HOME=/tmp/blhome; mkdir -p /tmp/blhome; cd /tmp/blhome; timeout $SECS stdbuf -oL desktop wayland blender --factory-startup 2>&1; echo BLENDER_RC=\$?; echo PROGRAM_RC=0'" ;;
	vkcube-trace) CMD="debian 'export $G; (timeout $SECS desktop wayland vkcube --wsi wayland --c 100000 > /tmp/vkcube.out 2>&1 &); sleep ${1:-30}; python3 /run/rvgpu/bin/stageb-trace.py 8 vkcube 2>&1; echo PROGRAM_RC=\$?'" ;;
	qtquick|qtquick-vulkan|gtk4|chromium)
	           # (the guest script ends the program by timeout: `desktop wayland` does not return under node-machine, so rc 124 is the normal end)
	           [ -n "$SECS_SET" ] || { case $PROG in chromium) SECS=240 ;; *) SECS=90 ;; esac; }
	           [ -n "$TIMEOUT_SET" ] || TIMEOUT=$(( (SECS + 240) * 1000 ))
	           CMD="debian 'export $G; bash /run/rvgpu/matrix/gpu-matrix-guest.sh $PROG $SECS 2>&1; echo PROGRAM_RC=\$?'" ;;
	sh:*)      CMD=${PROG#sh:} ;;
	dbg:*)     CMD="debian 'export $G; ${PROG#dbg:}; echo PROGRAM_RC=\$?'" ;;
	*) echo "gpu-nm: no program $PROG (es2gears eglinfo glmark2 vkr-bench vulkaninfo venus-fail vkcube mpv qtquick qtquick-vulkan gtk4 chromium blender-bg blender sh:CMD dbg:CMD)" >&2; exit 2 ;;
esac

export WL_DUMP_DIR=${WL_DUMP_DIR:-$N/shots} WL_DUMP_MAX=${WL_DUMP_MAX:-4}
export RVGPU_NM_DIR=${RVGPU_NM_DIR:-$N/shots}
rm -f $N/shots/*.png
OUT=$N/last.out; ERR=$N/last.err
opts=(--disk "$DISK" --server "$URL" --append "autostart=0 $GPU_NM_APPEND" --hook $HOOK)
[ -n "$VERBOSE" ] && opts+=(--verbose)
set +e
if [ -n "$VERBOSE" ]; then
	$NODE $NODE_FLAGS tools/node-machine.js --runtime $N/rt --kernel $N/vmlinux-wasm64_nommu.wasm \
		--initramfs $N/initramfs-wasm64_nommu.cpio.gz --initramfs $N/extra.cpio.gz --timeout "$TIMEOUT" "${opts[@]}" "$CMD" 2> >(tee $ERR >&2) | tee $OUT
	RC=${PIPESTATUS[0]}
else
	$NODE $NODE_FLAGS tools/node-machine.js --runtime $N/rt --kernel $N/vmlinux-wasm64_nommu.wasm \
		--initramfs $N/initramfs-wasm64_nommu.cpio.gz --initramfs $N/extra.cpio.gz --timeout "$TIMEOUT" "${opts[@]}" "$CMD" > $OUT 2> $ERR
	RC=$?
fi
set -e
exec $NODE gpu/tools/gpu-nm-report.js "$PROG" "$RC" "$OUT" "$ERR" "$N/shots"
