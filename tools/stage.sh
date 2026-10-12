#!/bin/bash
# Rebuild everything of ours that goes onto the Linux/Wasm system and repack
# the initramfs served by the dev server. Run inside the build VM.

set -e

S="$(realpath "$(dirname "$0")/..")"
# Where the builds and what is served are (RVWASM_WORK), the main checkout (RVWASM_MAIN) and the GPU
# host's builds (GPUHOST): the build VM's by default; the build container (docker/) sets its own.
W="${RVWASM_WORK:-$HOME/rvwasm}"
GPUHOST="${GPUHOST:-$HOME/gpuhost}"
# What is built from this source tree is kept apart from what is built from
# another (a second checkout of the repository that someone else works in):
# the directories of the tree in $MAIN have their old names, another tree's
# end in a number made from its path. What is staged, though, is one thing
# for everybody: see tools/with-browser.sh, which this is to be run under.
MAIN="${RVWASM_MAIN:-$HOME/dev/rvwasm}"
X=
[ "$S" = "$MAIN" ] || X="-$(echo "$S" | cksum | cut -d' ' -f1)"
LLVM="$W/linux-wasm/workspace/install/llvm/bin"

mkdir -p "$W/build-probes$X"
# Known noise from the wasm64 toolchain: the long double helpers (see probes/fp_probe.c).
filter='signature mismatch|^>>> |^$|not yet stable'

# Native wasm programs: the probes and the emulator.
for p in hello mmap_probe clone_probe fp_probe spawn_sig_probe shm_probe sched_bench; do
	"$S/tools/wasm-cc" -o "$W/build-probes$X/$p" "$S/probes/$p.c" 2>&1 | grep -v -E "$filter" || true
done
# (RV_WASM_CFLAGS: more flags for it, such as -DRV_HEAPCHECK for a build that hunts memory corruption;
# such a build is kept apart, so that the next ordinary one is not made of its objects.)
RVW="$W/build-rv-wasm$X${RV_WASM_CFLAGS:+-flags}"
[ -z "${RV_WASM_CFLAGS-}" ] || rm -rf "$RVW"
# (Linked again when the C library is newer than it: make does not know that rv is made of it. Fixes to musl, such
# as patches/musl-wasm64.diff's, reach rv only so.)
LIBC="$W/linux-wasm/workspace/install/musl-${LW_VARIANT:-wasm64_nommu}/lib/libc.a"
if [ -e "$RVW/rv" ] && [ "$LIBC" -nt "$RVW/rv" ]; then rm -f "$RVW/rv"; fi
make -s -j"${JOBS:-6}" -C "$S/rv" O="$RVW" CC="$S/tools/wasm-cc" AR="$LLVM/llvm-ar" CFLAGS="${RV_WASM_CFLAGS-}" 2>&1 |
	grep -v -E "$filter" || true

# hostnode (hostnode/): the program that hands over to JavaScript.
"$S/tools/wasm-cc" -Wall -o "$W/build-probes$X/hostnode" "$S/hostnode/hostnode.c" 2>&1 | grep -v -E "$filter" || true
"$S/tools/wasm-cc" -o "$W/build-probes$X/multi_signal_probe" "$S/probes/multi_signal_probe.c" 2>&1 | grep -v -E "$filter" || true
"$S/tools/wasm-cc" -o "$W/build-probes$X/siginfo_probe" "$S/probes/siginfo_probe.c" 2>&1 | grep -v -E "$filter" || true
# The linux-wasm audit's reproducers (tests/linux-wasm-audit, check.sh says what each should print), as /audit/NAME.
mkdir -p "$W/build-probes$X/audit"
for c in "$S"/tests/linux-wasm-audit/*.c; do
	n=$(basename "$c" .c)
	extra=
	[ "$n" = cas_p ] && extra="-DMUSL_ATOMIC -I$W/linux-wasm/workspace/src/musl/arch/wasm"
	"$S/tools/wasm-cc" $extra -o "$W/build-probes$X/audit/$n" "$c" 2>&1 | grep -v -E "$filter" || true
done

# Native build of the emulator and the riscv64 guest tests.
make -s -j"${JOBS:-6}" -C "$S/rv" O="$W/build-rv$X"
make -s -C "$S/tests" O="$W/build-tests$X" all

rm -rf "$W/stage"
mkdir -p "$W/stage/bin" "$W/stage/rv"
cp "$W/build-probes$X/hello" "$W/stage/bin/wasm-hello"
cp "$W/build-probes$X/mmap_probe" "$W/build-probes$X/clone_probe" "$W/build-probes$X/fp_probe" \
	"$W/build-probes$X/spawn_sig_probe" "$W/build-probes$X/shm_probe" "$W/build-probes$X/sched_bench" "$W/stage/bin/"
cp "$RVW/rv" "$W/stage/bin/rv"
# Native wasm CoreMark, if it has been built (tools/build-coremark.sh).
if [ -x "$W/build-probes/coremark-wasm" ]; then
	cp "$W/build-probes/coremark-wasm" "$W/stage/bin/coremark-wasm"
fi
# hostnode: the program, its JavaScript, and Node's own library, which is taken out of the Node.js of the
# Debian system (hostnode/extract.js: that node runs it, here under qemu) once for every version of either.
H="$W/stage/usr/lib/hostnode"
mkdir -p "$H"
cp "$W/build-probes$X/hostnode" "$H/hostnode"
ln -s ../usr/lib/hostnode/hostnode "$W/stage/bin/hostnode"
cp "$W/build-probes$X/multi_signal_probe" "$W/build-probes$X/siginfo_probe" "$W/stage/bin/"
mkdir -p "$W/stage/audit" && { cp "$W/build-probes$X/audit/"* "$W/stage/audit/" || true; }
cp -r "$S/hostnode/js/." "$H/"
# (RV_DEBIAN_ROOT: another root that has Node, e.g. the release image's.)
D="${RV_DEBIAN_ROOT:-$W/debian-root-kde}"
if [ -x "$D/usr/bin/node" ]; then
	key="$(cat "$S/hostnode/extract.js" "$D"/usr/lib/riscv64-linux-gnu/libnode.so.* | cksum | cut -d' ' -f1)"
	if [ ! -e "$W/hostnode-lib/$key/node.json" ]; then
		rm -rf "$W/hostnode-lib"
		mkdir -p "$W/hostnode-lib/$key"
		/usr/bin/env -i QEMU_LD_PREFIX="$D" /usr/bin/qemu-riscv64 -L "$D" "$D/usr/bin/node" --no-warnings --expose-internals \
			"$S/hostnode/extract.js" "$W/hostnode-lib/$key"
	fi
	cp -r "$W/hostnode-lib/$key/." "$H/"
fi
# (Its tests next to it, so that they are also there where Debian finds hostnode: tools/debian-enter.)
mkdir -p "$H/tests"
cp -r "$S/tests/hostnode/." "$H/tests/"
# The window server (wlserver/): JavaScript that hostnode runs, where Debian finds it as /run/wlserver.
mkdir -p "$W/stage/usr/lib/wlserver"
cp -r "$S/wlserver/." "$W/stage/usr/lib/wlserver/"
cp "$S/tools/x/desktop" "$W/stage/usr/lib/wlserver/desktop"	# (debian-enter puts it over the disk's own)
chmod 755 "$W/stage/usr/lib/wlserver/desktop"
cp -r "$S/tools/x/xdg" "$W/stage/usr/lib/wlserver/xdg"	# (the desktop's defaults, until the disk has them)
# The GPU server (gpu/server/): JavaScript that hostnode runs, where Debian finds it as /run/rvgpu
# (`RVGPU=1 desktop wayland PROGRAM`, gpu/PROTOCOL.md). Mesa's demos (Debian's mesa-utils-bin, not on the
# disks) go with it in bin/ when they have been fetched into $W/gpuguest/x (gpu/native-vtest.sh says how).
mkdir -p "$W/stage/usr/lib/rvgpu/bin"
cp "$S/gpu/server/"*.js "$W/stage/usr/lib/rvgpu/"
# The VA-API driver for guest players (gpu/vdec/driver: the page's WebCodecs decodes), /run/rvgpu/va in Debian;
# it needs libva's headers (libva-dev of trixie riscv64, unpacked under VA_INC; the setup script fetches them).
VA_INC=${VA_INC:-$HOME/vdec/x/usr/include}
# (or libva's headers out of the release image's root, which has libva-dev: tools/build-debian-image.sh release-root;
# only those, its C library's are another glibc than the cross compiler's)
if [ ! -e "$VA_INC/va/va_backend.h" ] && [ -e "${RV_DEBIAN_ROOT:-$W/debian-root-release}/usr/include/va/va_backend.h" ]; then
	VA_INC="$W/va-include"
	rm -rf "$VA_INC" && mkdir -p "$VA_INC"
	cp -r "${RV_DEBIAN_ROOT:-$W/debian-root-release}/usr/include/va" "$VA_INC/va"
fi
if [ -e "$VA_INC/va/va_backend.h" ]; then
	VA_INC=$VA_INC "$S/gpu/vdec/driver/build.sh" "$W/stage/usr/lib/rvgpu/va" > /dev/null
else
	echo "stage: no libva headers in $VA_INC: no VA-API driver (guest players decode in software)"
fi
cp "$S/gpu/tools/dmabuf-test.py" "$W/stage/usr/lib/rvgpu/bin/dmabuf-test"
for b in es2gears_wayland eglgears_wayland eglinfo es2_info peglgears; do
	[ -e "$W/gpuguest/x/usr/bin/$b.riscv64-linux-gnu" ] && cp "$W/gpuguest/x/usr/bin/$b.riscv64-linux-gnu" "$W/stage/usr/lib/rvgpu/bin/$b"
done
chmod 755 "$W/stage/usr/lib/rvgpu/bin/"*
# hostopen (hostopen/): web addresses from the machine to the browser it runs in; Debian finds it as /run/hostopen.
mkdir -p "$W/stage/usr/lib/hostopen"
cp -r "$S/hostopen/." "$W/stage/usr/lib/hostopen/"
# blender-gpu (blenderweb/guest/): Blender on the host's GPU as a window of the desktop; with the window server,
# whose client it is: /run/wlserver/blenderweb. (The page's side is tools/www/blenderweb.js; Blender itself is
# served by blenderweb/serve.py.)
mkdir -p "$W/stage/usr/lib/wlserver/blenderweb"
cp -r "$S/blenderweb/guest/." "$W/stage/usr/lib/wlserver/blenderweb/"
chmod 755 "$W/stage/usr/lib/wlserver/blenderweb/blender-gpu"

# The first riscv64 program rv ran: no libc, only write and exit (tests/hello-nolibc.c).
if [ ! -x "$W/guest/hello" ]; then
	mkdir -p "$W/guest"
	riscv64-linux-gnu-gcc -O2 -static -nostdlib -march=rv64im -mabi=lp64 -o "$W/guest/hello" "$S/tests/hello-nolibc.c"
fi
cp "$W/guest/hello" "$W/stage/rv/hello"
for t in arith-O2 arith-c-Os mm-c atomic-gc fp-gc signal-gc codegen bitmanip-loop hwprobe; do
	cp "$W/build-tests$X/$t" "$W/stage/rv/$t"
done
# riscv64 BusyBox, if it has been built (see tools/build-busybox-rv.sh), plus
# /rvbin with one link per applet. The name has to start with "busybox" for it
# to act as the multi-call binary.
if [ -x "$W/busybox-rv/busybox" ]; then
	cp "$W/busybox-rv/busybox" "$W/stage/bin/busybox-rv64"
	mkdir -p "$W/stage/rvbin"
	for applet in $(qemu-riscv64 "$W/busybox-rv/busybox" --list); do
		ln -s /bin/busybox-rv64 "$W/stage/rvbin/$applet"
	done
fi
# Our /init replaces upstream's: a later entry in the archive wins.
cp "$S/tools/initramfs-init" "$W/stage/init"
chmod 755 "$W/stage/init"

cp "$S/tests/smoke.sh" "$S/tests/bench.sh" "$S/tests/debian.sh" "$S/tests/procmem.sh" "$S/tests/plasma-time.sh" "$S/tests/devtools.sh" "$S/tests/jitcache.sh" "$S/tests/selfmod.sh" "$S/tests/fp.sh" "$S/tests/js/jsbench.js" "$W/stage/rv/"
cp "$S/tools/debian-enter" "$W/stage/bin/debian"
# Name servers: the ones this machine uses (the guest reaches them through the relay's address translation).
mkdir -p "$W/stage/etc"
grep '^nameserver' /run/systemd/resolve/resolv.conf > "$W/stage/etc/resolv.conf" 2> /dev/null ||
	echo "nameserver 1.1.1.1" > "$W/stage/etc/resolv.conf"

# Dynamic linking: the riscv64 loaders and libraries where programs expect
# them, and the tests (tools/build-dyn-tests.sh). On this system the riscv64
# files are the only shared libraries, so no sysroot is needed.
if [ -d "$W/build-dyn" ]; then
	G=/usr/riscv64-linux-gnu/lib
	mkdir -p "$W/stage/lib"
	cp "$W/sysroot-musl/usr/lib/libc.so" "$W/stage/lib/ld-musl-riscv64.so.1"
	cp -L "$G/ld-linux-riscv64-lp64d.so.1" "$W/stage/lib/"
	cp "$W"/build-dyn/* "$W/stage/rv/"
	# The thread tests (tools/build-thread-tests.sh) need the same libraries.
	if [ -d "$W/build-threads" ]; then
		cp "$W"/build-threads/* "$W/stage/rv/"
	fi
	if [ -x "$W/busybox-rv-dyn/busybox" ]; then
		cp "$W/busybox-rv-dyn/busybox" "$W/stage/bin/busybox-rv64-dyn"
	fi
	# Every glibc library the staged programs name, and what those name in turn.
	todo=("$W"/stage/rv/* "$W/stage/bin/busybox-rv64-dyn")
	while [ ${#todo[@]} -gt 0 ]; do
		next=()
		for f in "${todo[@]}"; do
			for lib in $(riscv64-linux-gnu-readelf -d "$f" 2> /dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
				# (Only real ELF files: glibc's libc.so is a linker script.)
				if [ ! -e "$W/stage/lib/$lib" ] && riscv64-linux-gnu-readelf -h "$G/$lib" > /dev/null 2>&1; then
					cp -L "$G/$lib" "$W/stage/lib/$lib"
					next+=("$W/stage/lib/$lib")
				fi
			done
		done
		todo=("${next[@]}")
	done
	riscv64-linux-gnu-strip --strip-unneeded "$W"/stage/lib/*.so* 2> /dev/null || true
fi

# The frame buffer test, built here; other guest programs dropped into
# ~/rvwasm/guest-extra are staged as they are.
mkdir -p "$W/guest-extra"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/fbtest" "$S/tests/fb/fbtest.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/live_map" "$S/tests/mm/live_map.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/scattered_io" "$S/tests/mm/scattered_io.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/madvise" "$S/tests/mm/madvise.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/reserve" "$S/tests/mm/reserve.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/mremap_file" "$S/tests/mm/mremap_file.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -pthread -o "$W/guest-extra/selfmod" "$S/tests/mm/selfmod.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/fma" "$S/tests/mm/fma.c"
# ... and what they print on a system that is known to be right, to compare with (tests/selfmod.sh, tests/fp.sh).
{
	qemu-riscv64 "$W/guest-extra/selfmod"
	echo "twice: 560000"	# (the one case that is not qemu-riscv64's to get right: see the test)
} > "$W/guest-extra/selfmod.expected"
qemu-riscv64 "$W/guest-extra/fma" > "$W/guest-extra/fma.expected"
qemu-riscv64 "$W/build-tests$X/fp-gc" > "$W/guest-extra/fp-gc.expected"
riscv64-linux-gnu-gcc -O2 -Wall -static -pthread -o "$W/guest-extra/seccomp" "$S/tests/seccomp/seccomp.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/seccomp-strict" "$S/tests/seccomp/strict.c"
riscv64-linux-gnu-gcc -O2 -Wall -static -o "$W/guest-extra/chroot" "$S/tests/chroot/chroot.c"
if [ -d "$W/guest-extra" ]; then
	cp -r "$W/guest-extra/." "$W/stage/rv/"
fi
chmod 755 "$W"/stage/bin/* "$W"/stage/rv/* "$H/hostnode"

# The dev page and our changes to the JS runtime.
cp "$S/tools/www/dev.html" "$S/tools/www/disk-worker.js" "$S/tools/www/watchdog.js" "$S/tools/www/display.js" "$S/tools/www/chrome.css" "$S/tools/www/panel.css" "$S/tools/www/plasma.js" "$S/tools/www/splash.js" "$S/tools/www/breeze.css" "$S/tools/www/hostopen.js" "$S/tools/www/hostopen.css" "$S/tools/www/blenderweb.js" "$S/tools/www/background.js" "$S/tools/www/audio.js" "$S/tools/www/audio-worklet.js" "$W/www/"
# The page's GPU executor (gpu-exec.js) and virglrenderer built to wasm for it (gpu/vrend/build.sh puts it in
# $GPUHOST/out (~/gpuhost/out in the build VM); without it the executor cannot start and GPU programs are not offered one).
cp "$S/tools/www/gpu-exec.js" "$W/www/"
# The video decoder for guest players (docs/video-decode.md): page glue and its Worker.
cp "$S/tools/www/vdec.js" "$S/tools/www/vdec-worker.js" "$W/www/"
if [ -e "$GPUHOST/out/vrend.wasm" ]; then
	mkdir -p "$W/www/vrend"
	cp "$GPUHOST/out/vrend.js" "$GPUHOST/out/vrend.wasm" "$W/www/vrend/"
fi
# gpu/webgpu's executor (?exec=webgpu): rvgpu-worker.js and rvgpu.{js,wasm} (vfront on zink on wgvk), as
# gpu/webgpu/build/wasm.sh built them for this tree (build name = the tree's directory name).
RVG="$W/webgpu/b/$(basename "$S")/wasm/src/webgpu/rvgpu"
if [ -e "$RVG/rvgpu.wasm" ]; then
	mkdir -p "$W/www/rvgpu"
	cp "$RVG/rvgpu.js" "$RVG/rvgpu.wasm" "$W/www/rvgpu/"
	cp "$S/tools/www/rvgpu-worker.js" "$W/www/"
fi
# Where the kernel's variables are, for tools that read them out of the machine's memory (tools/desktop-drive.js).
grep -E " (jiffies_64|tick_do_timer_cpu|__per_cpu_offset|runqueues|nr_cpu_ids|__cpu_online_mask|raised_irqs|local_timer_expiries|wasm_flags_changes|wasm_flags_changed|wasm_flags_report|wasm_cpuflags)$" \
	"$W/linux-wasm/workspace/build/kernel-$LW_VARIANT/System.map" > "$W/www/kernel-symbols.txt"
# (From the runtime as it came: the patches of another tree, or of this one as it was, are not built on.)
git -C "$W/linux-wasm" checkout -q runtime
python3 "$S/tools/patch-runtime.py" "$W/linux-wasm/runtime" > /dev/null

rm -f "$W/www/initramfs-wasm64_nommu.cpio.gz"
"$S/tools/mk-initramfs.sh" "$W/stage" "$W/www/initramfs-wasm64_nommu.cpio.gz"
