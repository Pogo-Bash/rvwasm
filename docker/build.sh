#!/bin/bash
# The build container's steps (docker/Dockerfile.build; docs/docker.md). Everything is built into
# $RVWASM_WORK (/work, a volume), laid out as ~/rvwasm is in the build VM, with the scripts in tools/
# unchanged; the source tree is /src.
#
#   docker/build.sh STEP...
#
#   toolchain   linux-wasm's side: the kernel (with patches/kernel-*.diff, config rvwasm), musl (with
#               patches/musl-wasm64.diff), BusyBox and the boot image it is the base of; and /work/www with
#               the JS runtime and the kernel in it. (Each part only if it is not built yet; `kernel`, `musl`,
#               `busybox` alone build that part again.)
#   rv          riscv64 BusyBox, the dynamic-linking and thread tests (tools/build-busybox-rv.sh,
#               build-dyn-tests.sh, build-thread-tests.sh): what tools/stage.sh puts beside rv
#   gpu         virglrenderer's vrend for the page (gpu/vrend/build.sh)
#   stage       tools/stage.sh: rv (native and wasm), hostnode, the window and GPU servers, the boot image
#   debian      the Debian disks (tools/build-debian-image.sh): root, x-root, kde-root, dev kde, chromium kde, then
#               `disk kde 8G daily` and `home`. Needs the build-debian service (privileged) and riscv64 in
#               the host's binfmt_misc with the F flag. Hours, and ~25 GB in /work.
#   release     the release image's disk (tools/nq-bundle.sh, build-debian-image.sh release-root, disk release
#               8G): needs `debian` first and nativeqt's build at $NQ_SYSROOT (build-debian service)
#   pack        tools/pack-www.sh /work/www /dist: what the run image serves (RV_DISKS, RV_IMAGES)
#   all         toolchain rv gpu stage pack (not debian)
#   shell       a shell, with all of the above set up

set -e

S=/src
W="${RVWASM_WORK:-/work}"
LW="$W/linux-wasm"
V="${LW_VARIANT:-wasm64_nommu}"
KERNEL_COMMIT=039e5f3e583f56f329657d1fe9945510dba10f41
export JOBS="${JOBS:-6}"
export LW_JOBS_KERNEL_COMPILE="$JOBS" LW_JOBS_MUSL_COMPILE="$JOBS" LW_JOBS_BUSYBOX_COMPILE="$JOBS"

say() { echo "=== $*"; }

# A clone of the image's linux-wasm (our commit on top), with the image's LLVM.
setup() {
	if [ ! -d "$LW/.git" ]; then
		git clone -q -b rvwasm /opt/linux-wasm "$LW"
	fi
	mkdir -p "$LW/workspace/install" "$LW/workspace/src"
	ln -sfn /opt/linux-wasm/workspace/install/llvm "$LW/workspace/install/llvm"
	# (Git refuses to work in a tree another user owns: /src is the host's.)
	git config --global --add safe.directory '*'
}

# The kernel's patches, in this order; the source is made again when they change (a mark file with their sum).
KERNEL_PATCHES="kernel-rvwasm.diff kernel-memsize.diff kernel-lwfix.diff kernel-audio.diff"
kernel_sum() { (cd "$S/patches" && cat $KERNEL_PATCHES) | cksum | cut -d' ' -f1; }

kernel() {
	local k="$LW/workspace/src/kernel"
	local sum
	sum=$(kernel_sum)
	if [ ! -e "$k/.rv-patches-$sum" ]; then
		say "kernel: fetch $KERNEL_COMMIT, patches $KERNEL_PATCHES"
		rm -rf "$k"
		git init -q "$k"
		git -C "$k" fetch -q --depth 1 https://github.com/joelseverin/linux.git "$KERNEL_COMMIT"
		git -C "$k" checkout -q FETCH_HEAD
		for p in $KERNEL_PATCHES; do
			git -C "$k" apply "$S/patches/$p"
		done
		touch "$k/.rv-patches-$sum"
	fi
	say "kernel: build"
	LW_KERNEL_CONFIG=rvwasm "$LW/linux-wasm.sh" build-kernel > "$W/kernel.log" 2>&1 || { tail -40 "$W/kernel.log"; exit 1; }
}

musl() {
	local m="$LW/workspace/src/musl"
	if [ ! -d "$m/.git" ]; then
		say "musl: fetch"
		"$LW/linux-wasm.sh" fetch-musl > "$W/musl-fetch.log" 2>&1 || { tail -20 "$W/musl-fetch.log"; exit 1; }
		git -C "$m" apply "$S/patches/musl-wasm64.diff"
	fi
	say "musl: build"
	rm -rf "$LW/workspace/build/musl-$V"	# (its makefile does not follow arch/wasm/*.h)
	"$LW/linux-wasm.sh" build-musl > "$W/musl.log" 2>&1 || { tail -40 "$W/musl.log"; exit 1; }
}

busybox() {
	if [ ! -d "$LW/workspace/src/busybox/.git" ]; then
		say "busybox: fetch"
		"$LW/linux-wasm.sh" fetch-busybox > "$W/busybox-fetch.log" 2>&1 || { tail -20 "$W/busybox-fetch.log"; exit 1; }
	fi
	say "busybox: build, and the boot image's base"
	for a in build-busybox-kernel-headers build-busybox build-initramfs; do
		"$LW/linux-wasm.sh" "$a" > "$W/busybox.log" 2>&1 || { tail -40 "$W/busybox.log"; exit 1; }
	done
}

# What the dev server serves that is not staged: the JS runtime (stage.sh patches it in place) and the kernel.
www() {
	mkdir -p "$W/www"
	for f in linux.js linux-worker.js xterm.js xterm.css bright.css server.py; do
		ln -sfn "$LW/runtime/$f" "$W/www/$f"
	done
	ln -sfn "$LW/workspace/install/kernel-$V/vmlinux.wasm" "$W/www/vmlinux-$V.wasm"
}

toolchain() {
	[ -e "$LW/workspace/install/kernel-$V/vmlinux.wasm" ] && [ -e "$LW/workspace/src/kernel/.rv-patches-$(kernel_sum)" ] &&
		[ ! "$LW/workspace/src/kernel/.rv-patches-$(kernel_sum)" -nt "$LW/workspace/install/kernel-$V/vmlinux.wasm" ] || kernel
	[ -e "$LW/workspace/install/musl-$V/lib/libc.a" ] || musl
	[ -e "$LW/workspace/install/initramfs-$V/initramfs.cpio.gz" ] || busybox
	www
}

rv() {
	say "rv: riscv64 BusyBox, dynamic-linking and thread tests"
	"$S/tools/build-busybox-rv.sh"
	"$S/tools/build-busybox-rv.sh" dynamic
	"$S/tools/build-dyn-tests.sh"
	"$S/tools/build-thread-tests.sh"
}

gpu() {
	say "gpu: vrend"
	# (A copy of gpu/vrend: build.sh writes generated headers and stubs into its sources, as it does in the
	# build VM's ~/gpuhost/vrend-src.)
	rm -rf "$GPUHOST/vrend-src" && mkdir -p "$GPUHOST" && cp -r "$S/gpu/vrend" "$GPUHOST/vrend-src"
	SRC="$GPUHOST/vrend-src" bash "$GPUHOST/vrend-src/build.sh" > "$W/vrend.log" 2>&1 || { tail -40 "$W/vrend.log"; exit 1; }
	ls -la "$GPUHOST/out/vrend.wasm"
}

stage() {
	say "stage"
	"$S/tools/stage.sh" > "$W/stage.log" 2>&1 || { tail -40 "$W/stage.log"; exit 1; }
	ls -la "$W/www/initramfs-$V.cpio.gz"
}

debian() {
	say "debian: root, x-root, kde-root, dev kde (hours)"
	local d="$S/tools/build-debian-image.sh"
	[ -x "$W/debian-root/bin/bash" ] || "$d" root
	[ -x "$W/debian-root-x/usr/bin/xinit" ] || "$d" x-root
	[ -x "$W/debian-root-kde/usr/bin/plasmashell" ] || "$d" kde-root
	[ -x "$W/debian-root-kde/usr/bin/node" ] || "$d" dev kde
	[ -x "$W/debian-root-kde/usr/lib/chromium/chromium" ] || "$d" chromium kde
	say "debian: disk kde 8G daily, home"
	"$d" disk kde 8G daily
	[ -e "$W/www/disk/home/manifest.json" ] || "$d" home
}

# The release image (README.md "Run it"; tools/build-debian-image.sh release-root): the native plasmashell from
# the nativeqt build mounted at $NQ_SYSROOT (tools/nq-bundle.sh), the release root on top of the `debian` step's
# KDE root, and its disk. docker/release.sh does the same in the build VM with what is built there.
release() {
	say "release: the native plasmashell, the release root, its disk"
	RV_DEBIAN_ROOT="$W/debian-root-kde" "$S/tools/nq-bundle.sh" "${NQ_SYSROOT:-/nq-sysroot}" "$S/tools/x/nq-share" "$W/nq-bundle"
	"$S/tools/build-debian-image.sh" release-root
	"$S/tools/build-debian-image.sh" disk release 8G
	rm -f "$W/disks/release.ext4"	# (its pieces are what is served)
	[ -e "$W/www/disk/home/manifest.json" ] || "$S/tools/build-debian-image.sh" home
}

pack() {
	say "pack: /dist"
	"$S/tools/pack-www.sh" "$W/www" /dist
	# (Owned by whoever owns the source tree, not by the container's root.)
	chown -R "$(stat -c %u:%g "$S")" /dist
}

setup
# A Debian root with Node mounted from outside (docker-compose.yml: RV_DEBIAN_ROOT_DIR) for tools/stage.sh, or else
# the volume's own (the `debian` step's).
if [ -n "$RV_DEBIAN_ROOT" ] && [ ! -x "$RV_DEBIAN_ROOT/usr/bin/node" ]; then
	unset RV_DEBIAN_ROOT
fi
[ $# -gt 0 ] || set -- all
for step in "$@"; do
	case "$step" in
	all) toolchain; rv; gpu; stage; pack ;;
	toolchain | kernel | musl | busybox | www | rv | gpu | stage | debian | release | pack) "$step" ;;
	shell) exec bash ;;
	*) echo "docker/build.sh: no step $step" >&2; exit 2 ;;
	esac
done
