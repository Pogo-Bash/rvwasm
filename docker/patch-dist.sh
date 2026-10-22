#!/bin/bash
# Brings a dist up to date with this tree's page files and boot script without building anything. The build's dist is
# made from the build VM's copy of the tree, which can be a release behind; this is the one place that says what the
# live site has from *this* tree instead:
#   - www/dev.html, www/probe.html, www/disk-worker.js, www/display.js, www/gpu-exec.js, www/vdec-worker.js, www/hostopen.js and www/hostopen.css are this tree's (tools/www/);
#   - www/linux.js and www/linux-worker.js get the patches of tools/patch-runtime.py they lack (each patch checks its own marker, so
#     patched files are left alone; the dist's runtime can be a few patches behind this tree: patch 27, Firefox, needs 26);
#   - the disks' manifests get their boot list (docker/boot, tools/r2-publish.py --boot-only);
#   - the boot image gets this tree's /init (tools/initramfs-init) and desktop script (tools/x/desktop) appended to the initramfs (the later entry wins, as in
#     tools/mk-initramfs.sh), unless its last /init already is that file; and `debian` (tools/debian-enter: it binds the native perl of the
#     nqs disk over /usr/bin/perl, so the boot image's copy has to be this tree's). (The init writes nameserver 10.0.5.1 into
#     /etc/resolv.conf: dns= on the kernel command line.)
# tools/deploy.sh runs this in the build VM on the dist before it copies it, and docker/release.sh's dist step runs it,
# so a plain deploy gives the live site exactly this. Running it again changes nothing.
#
#   - the boot image also gets wlserver/xwm.js (/usr/lib/wlserver/xwm.js), whatever RV_ONLY says, like the GPU server files;
#   - the boot image also gets gpu/server/*.js (the GPU server, /usr/lib/rvgpu) whatever RV_ONLY says: the same rule, appended entries that win;
#   - jitcache.js (the page's store of translated code, tools/patch-runtime.py 29) is this tree's, and with RV_BIN=FILE (the emulator,
#     a file built from this tree: rv/Makefile's O/rv) the boot image gets FILE as /bin/rv and www/rv.sha256 its SHA-256, which is what
#     lets the page's store serve that one program and no other (without RV_BIN the dist has none and the page keeps no store);
#
#   - the WebGPU executor (?exec=webgpu; VK ship): www/vdec.js and www/rvgpu-worker.js are this tree's; with RV_RVGPU_DIR=DIR (rvgpu.js and
#     rvgpu.wasm of a build, which must be stamped and clean: tools/rvgpu-stamp-check.py) www/rvgpu/ is that build; with RV_VENUS_DIR=DIR
#     (gpu/tools/venus-stage.sh's output: the venus driver, lavapipe, vulkaninfo, vkcube and their ICD files) the boot image gets them as
#     /usr/lib/rvgpu/venus, plus this tree's stageb-trace and the app matrix's guest script. None of this runs unless a visitor asks for
#     ?exec=webgpu (the page's default executor stays vrend: rv-site.json "gpu_exec", docs/deploy.md).
#
#     RV_GPU_BINS=DIR adds the GL test programs (es2gears_wayland, eglgears_wayland, eglinfo, es2_info, peglgears) as /usr/lib/rvgpu/bin.
#
#   - RV_FILES="dst:src ..." adds files of the tree to the boot image even under RV_ONLY=1 (dst in the image, src in the tree);
#
#   [RV_BIN=FILE] [RV_RVGPU_DIR=DIR] [RV_VENUS_DIR=DIR] [RV_GPU_BINS=DIR] docker/patch-dist.sh DIST        (DIST/www/dev.html, disk-worker.js and initramfs-wasm64_nommu.cpio.gz)
set -e
S="$(cd "$(dirname "$0")/.." && pwd)"
D=${1:?usage: patch-dist.sh DIST}
W=$D/www
[ -e "$W/dev.html" ] || { echo "patch-dist: no $W/dev.html" >&2; exit 1; }
for f in dev.html probe.html disk-worker.js display.js gpu-exec.js vdec.js vdec-worker.js rvgpu-worker.js jitcache.js hostopen.js hostopen.css; do
	cmp -s "$S/tools/www/$f" "$W/$f" || { cp "$S/tools/www/$f" "$W/$f"; echo "patch-dist: $f updated"; }
done
# The WebGPU executor's module (rvgpu.wasm + its loader), only from a stamped, clean build (tools/rvgpu-stamp-check.py, the rule of
# docker/release.sh): an unstamped one has no known source and stops the deploy.
if [ -n "${RV_RVGPU_DIR:-}" ]; then
	[ -s "$RV_RVGPU_DIR/rvgpu.wasm" ] && [ -s "$RV_RVGPU_DIR/rvgpu.js" ] || { echo "patch-dist: RV_RVGPU_DIR=$RV_RVGPU_DIR has no rvgpu.wasm and rvgpu.js" >&2; exit 1; }
	echo "patch-dist: rvgpu $(python3 "$S/tools/rvgpu-stamp-check.py" --line "$RV_RVGPU_DIR/rvgpu.wasm")" || { echo "patch-dist: rvgpu.wasm is not a stamped, clean build" >&2; exit 1; }
	mkdir -p "$W/rvgpu"
	for f in rvgpu.js rvgpu.wasm; do
		cmp -s "$RV_RVGPU_DIR/$f" "$W/rvgpu/$f" || { cp "$RV_RVGPU_DIR/$f" "$W/rvgpu/$f"; echo "patch-dist: rvgpu/$f updated"; }
	done
fi
# The runtime (tools/patch-runtime.py): the patches this tree has that the dist's linux.js and linux-worker.js lack.
if [ -e "$W/linux.js" ] && [ -e "$W/linux-worker.js" ] && command -v python3 > /dev/null; then
	python3 "$S/tools/patch-runtime.py" "$W" | grep -v ": already patched$" | sed "s/^/patch-dist: runtime, /" || true
fi
# The boot lists (docker/boot/NAME.json, tools/boot-list.py) into the disks' manifests, if they have object storage
# (`hashes`): the pieces a cold boot reads, which the page fetches ahead. The disk's `id` is not touched. Nothing changes
# when the manifest has the list already.
if [ -d "$D/disk" ] && command -v python3 > /dev/null; then
	(set -o pipefail; python3 "$S/tools/r2-publish.py" "$D" --boot-only --boot-dir "$S/docker/boot" | sed "s/^r2-publish:/patch-dist:/")
fi
img=$W/initramfs-wasm64_nommu.cpio.gz
# Files of this tree the boot image must carry, as "path in the image:path in the tree". The desktop script: the
# machine runs the boot image's copy, not the disk's (tools/debian-enter binds /usr/lib/wlserver/desktop over it).
files="init:tools/initramfs-init usr/lib/wlserver/desktop:tools/x/desktop bin/debian:tools/debian-enter usr/lib/wlserver/plasmashell.js:wlserver/plasmashell.js"
# RV_ONLY=1: the boot image keeps its /init, desktop script and debian as they are (a release of the emulator alone, which must not
# bring along the tree's other changes to the boot image).
[ -z "${RV_ONLY:-}" ] || files=
# The window server's X part (wlserver/xwm.js: Xwayland on demand, the shell's own X connection turned away) follows the tree whatever RV_ONLY says,
# like the GPU server below: it is one small file whose change has to reach the boot image on its own (a deploy of it must not carry /init, desktop...).
files="$files usr/lib/wlserver/xwm.js:wlserver/xwm.js"
# RV_FILES="dst:src ...": more of this tree's files for the boot image, whatever RV_ONLY says (a ship that means exactly these: the
# hostlinks ship's tools/x/xdg/config/mimeapps.list and rv-host-open.desktop, hostopen/bin/rv-host-open, the desktop script's cache stamp).
files="$files ${RV_FILES:-}"
# The GPU server (gpu/server/*.js, where stage.sh puts them: /usr/lib/rvgpu in the image, /run/rvgpu in Debian). Not part of what RV_ONLY holds
# back: they are the page's GPU path, which must be the tree's (a fix to them reaches the visitors only through the boot image).
for f in "$S"/gpu/server/*.js; do
	files="$files usr/lib/rvgpu/$(basename "$f"):gpu/server/$(basename "$f")"
done
# The window server's files whose changes reach visitors through the boot image alone (the image carries wlserver/ as of its build; a
# changed file is appended and wins). Like the GPU server above, not held back by RV_ONLY: a file listed here equals the tree's in the
# image except for the change that put it here. layershell.js + compositor.js: the usable area and a panel's visible part (a panel's
# transparent margin must not cut a maximized window's title bar), 2026-10-15.
for f in layershell.js compositor.js; do
	files="$files usr/lib/wlserver/$f:wlserver/$f"
done
# The venus tree (Vulkan through the page's WebGPU executor), with the tools that go with it: files of RV_VENUS_DIR, not of the tree.
if [ -n "${RV_VENUS_DIR:-}" ]; then
	[ -d "$RV_VENUS_DIR" ] && [ -e "$RV_VENUS_DIR/virtio_icd.json" ] || { echo "patch-dist: RV_VENUS_DIR=$RV_VENUS_DIR is not a venus tree (gpu/tools/venus-stage.sh)" >&2; exit 1; }
	for f in "$RV_VENUS_DIR"/*; do
		files="$files usr/lib/rvgpu/venus/$(basename "$f"):$f"
	done
	files="$files usr/lib/rvgpu/bin/stageb-trace:gpu/tools/stageb-trace.py usr/lib/rvgpu/matrix/gpu-matrix-guest.sh:gpu/tools/gpu-matrix-guest.sh"
	files="$files usr/lib/rvgpu/matrix/spin.qml:gpu/tools/matrix/spin.qml usr/lib/rvgpu/matrix/anim.html:gpu/tools/matrix/anim.html"
fi
# GL test programs for the GPU server (es2gears_wayland, eglinfo, ...: what tools/stage.sh puts in usr/lib/rvgpu/bin), files of RV_GPU_BINS.
if [ -n "${RV_GPU_BINS:-}" ]; then
	[ -d "$RV_GPU_BINS" ] || { echo "patch-dist: RV_GPU_BINS=$RV_GPU_BINS is not a directory" >&2; exit 1; }
	for f in "$RV_GPU_BINS"/*; do
		files="$files usr/lib/rvgpu/bin/$(basename "$f"):$f"
	done
fi
# (An absolute path is a file that is not in the tree: the emulator.)
if [ -n "${RV_BIN:-}" ]; then
	[ -s "$RV_BIN" ] || { echo "patch-dist: RV_BIN=$RV_BIN is not a file" >&2; exit 1; }
	files="$files bin/rv:$RV_BIN"
	sum=$(sha256sum "$RV_BIN" | cut -c1-64)
	[ "$(cat "$W/rv.sha256" 2> /dev/null)" = "$sum" ] || { echo "$sum" > "$W/rv.sha256"; echo "patch-dist: rv.sha256 updated"; }
fi
stale=
for e in $files; do
	dst=${e%%:*} src=${e#*:}
	[ "${src#/}" != "$src" ] || src=$S/$src
	size=$(wc -c < "$src")
	# (--to-stdout prints every entry of that name, the first ones first; the last is the one that counts)
	gzip -dc "$img" | cpio -i --to-stdout "$dst" 2> /dev/null | tail -c "$size" | cmp -s - "$src" || stale="$stale $e"
done
if [ -z "$stale" ]; then
	echo "patch-dist: the boot image has this tree's files already"
	exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gzip -dc "$img" > "$tmp/i.cpio"
mkdir "$tmp/root"
for e in $stale; do
	dst=${e%%:*} src=${e#*:}
	[ "${src#/}" != "$src" ] || src=$S/$src
	mkdir -p "$tmp/root/$(dirname "$dst")"
	# (a directory the image may not have yet gets its own entry, so that it exists whatever the unpacker does with missing parents)
	case $dst in
		usr/lib/rvgpu/venus/*|usr/lib/rvgpu/matrix/*)
			d=$(dirname "$dst")
			[ -e "$tmp/dirdone.$(echo "$d" | tr / _)" ] || {
				touch "$tmp/dirdone.$(echo "$d" | tr / _)"
				chmod 755 "$tmp/root/$d"
				touch -d "@${SOURCE_DATE_EPOCH:-0}" "$tmp/root/$d"
				(cd "$tmp/root" && echo "$d" | cpio -o --format=newc -A -O "$tmp/i.cpio" --owner=0:0 --reproducible 2> /dev/null)
			} ;;
	esac
	cp "$src" "$tmp/root/$dst"
	chmod 755 "$tmp/root/$dst"
	touch -d "@${SOURCE_DATE_EPOCH:-0}" "$tmp/root/$dst"  # (with --reproducible below: the same inputs give the same image)
	(cd "$tmp/root" && echo "$dst" | cpio -o --format=newc -A -O "$tmp/i.cpio" --owner=0:0 --reproducible 2> /dev/null)
	echo "patch-dist: the boot image's /$dst updated"
done
gzip -c "$tmp/i.cpio" > "$img.tmp"
mv "$img.tmp" "$img"
