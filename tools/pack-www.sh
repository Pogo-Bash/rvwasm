#!/bin/bash
# Collect what the machine's page needs out of a staged www/ directory (tools/stage.sh's, links followed)
# into OUT, for the run image (docker/Dockerfile.run) or any other static server:
#
#   OUT/server.py       the dev server (linux-wasm's runtime/server.py: COOP/COEP/no-store headers)
#   OUT/www/            the page, its scripts, the JS runtime, the kernel, the boot image, the GPU executors
#   OUT/disk/NAME/      the disks (tools/build-debian-image.sh disk/home), each as its pieces
#
#   tools/pack-www.sh [WWW] [OUT]       WWW: default $RVWASM_WORK/www (~/rvwasm/www); OUT: default ./dist
#
# RV_DISKS: the disks to take (default "daily home"; a missing one is left out with a warning).
# RV_IMAGES: boot images with Debian in them to take as well, e.g. "debian debian-x" (default none: the
# disks have Debian). Disks are hard links when WWW and OUT are on one file system, so packing costs no space.
# What is not the page's is left behind: test scripts, profiles, qemu-wasm, other disks.

set -e

: "${LW_VARIANT:=wasm64_nommu}"
WWW="$(realpath "${1:-${RVWASM_WORK:-$HOME/rvwasm}/www}")"
OUT="$(realpath -m "${2:-dist}")"
DISKS="${RV_DISKS-daily home}"

[ -e "$WWW/dev.html" ] || { echo "pack-www: no dev.html in $WWW (run tools/stage.sh first)" >&2; exit 1; }

rm -rf "$OUT/www" "$OUT/disk" "$OUT/server.py"
mkdir -p "$OUT/www" "$OUT/disk"

need() { # files that must be there
	for f in "$@"; do
		[ -e "$WWW/$f" ] || { echo "pack-www: $WWW/$f is missing" >&2; exit 1; }
		cp -L "$WWW/$f" "$OUT/www/$f"
	done
}
want() { # files that are there when what makes them has run
	for f in "$@"; do
		if [ -e "$WWW/$f" ]; then cp -RL "$WWW/$f" "$OUT/www/$f"; else echo "pack-www: no $f (left out)" >&2; fi
	done
}

# The page and its scripts: every file of tools/www that the staging put there (tools/stage.sh copies the ones
# the page uses), the JS runtime (patched by tools/patch-runtime.py), the kernel and the boot image.
S="$(realpath "$(dirname "$0")/..")"
need dev.html linux.js linux-worker.js xterm.js xterm.css \
	"vmlinux-$LW_VARIANT.wasm" "initramfs-$LW_VARIANT.cpio.gz" kernel-symbols.txt
for f in "$S"/tools/www/*; do
	f="$(basename "$f")"
	[ ! -f "$WWW/$f" ] || cp -L "$WWW/$f" "$OUT/www/$f"
done
# The GPU executors: vrend (gpu/vrend/build.sh) and rvgpu (gpu/webgpu/build/wasm.sh, ?exec=webgpu).
want vrend rvgpu rvgpu-worker.js
for image in ${RV_IMAGES-}; do
	need "initramfs-$image-$LW_VARIANT.cpio.gz"
done

# The server, beside the page rather than in it.
if [ -e "$WWW/server.py" ]; then
	cp -L "$WWW/server.py" "$OUT/server.py"
else
	cp "${LW_ROOT:-${RVWASM_WORK:-$HOME/rvwasm}/linux-wasm}/runtime/server.py" "$OUT/server.py"
fi

for d in $DISKS; do
	if [ ! -e "$WWW/disk/$d/manifest.json" ]; then
		echo "pack-www: no disk $d (left out)" >&2
		continue
	fi
	cp -al "$WWW/disk/$d" "$OUT/disk/$d" 2> /dev/null || { rm -rf "$OUT/disk/$d"; cp -RL "$WWW/disk/$d" "$OUT/disk/$d"; }
done

du -shL "$OUT/www" "$OUT/disk"/* 2> /dev/null || true
