#!/bin/bash
# Build the release and start it (README.md "Run it"), on the Linux computer with Docker that the machine is built
# on (the build VM: ~/rvwasm has the Debian KDE root, `dev kde` and `chromium kde` done, and nativeqt's build).
# From the Mac, tools/release-setup.sh copies this tree to the VM and runs this there.
#
#   docker/release.sh [STEP...]          (from the top of the tree; default: all)
#
#   bundle    the native plasmashell and its share/ (tools/nq-bundle.sh; NQ_SYSROOT, default
#             ~/rvwasm/nativeqt/sysroot) into ~/rvwasm/nq-bundle
#   root      the release root (tools/build-debian-image.sh release-root; sudo): ~/rvwasm/debian-root-release
#   disk      its disk, 8G (build-debian-image.sh disk release 8G) into ~/rvwasm/www/disk/release, and the home disk
#             if there is none; a new disk only when the root is newer than the disk
#   code      the build container (docker/build.sh toolchain rv gpu stage pack): the kernel with the sound card,
#             musl, BusyBox, rv, vrend, the boot image and the page, into ./dist (without disks); the release root
#             is mounted for hostnode's Node library and the video driver's headers
#   dist      the disks into ./dist (hard links), the WebGPU executor (?exec=webgpu) when one is built
#             (RVGPU_DIR, default ~/rvwasm/www/rvgpu), dist/RELEASE.txt
#   publish   the disks' pieces to object storage (Cloudflare R2), named by their sha256, and the dist's manifests
#             rewritten to say where (tools/r2-publish.py; docs/deploy.md "Pieces from object storage"). Only the
#             pieces that are not in the bucket yet are uploaded; the manifests are written last. Needs R2_ENDPOINT,
#             R2_BUCKET, R2_ACCESS_KEY_ID and R2_SECRET_ACCESS_KEY in the environment (never in files here:
#             tools/release-setup.sh passes them from the Mac on stdin) and RV_PIECE_BASE (where the page will fetch
#             pieces: https://disk.example.com/disk/); without them this step does nothing and the pieces are served
#             by the site itself.
#   up        docker compose up -d --build: fetch, www and relay (http://localhost:${RV_HTTP_PORT:-8080}/)
#   down      docker compose down
#   tarball   ./rvwasm-release.tar.gz of ./dist, for RV_RELEASE_URL (docker/fetch.sh)
#   all       bundle root disk code dist publish up
#
# Docker needs sudo in the build VM: the docker commands are run with it when plain `docker` is refused.
# Environment: RV_HTTP_PORT, RV_NET_PORT (docker-compose.yml), RV_BUILD_JOBS (6), RV_BUILD_MEM (8g),
# RV_RELEASE_ID (what RELEASE.txt says this is; tools/release-setup.sh passes the commit).
set -e
S="$(cd "$(dirname "$0")/.." && pwd)"
W="${RVWASM_WORK:-$HOME/rvwasm}"
R="$W/debian-root-release"
cd "$S"

SUDO=
docker info > /dev/null 2>&1 || SUDO=sudo
compose() {
	$SUDO env RV_DIST="$S/dist" RV_HTTP_PORT="${RV_HTTP_PORT:-8080}" RV_NET_PORT="${RV_NET_PORT:-8081}" \
		RV_BUILD_JOBS="${RV_BUILD_JOBS:-6}" RV_BUILD_MEM="${RV_BUILD_MEM:-8g}" RV_RELEASE_URL="${RV_RELEASE_URL-}" \
		RV_DEBIAN_ROOT_DIR="${RV_DEBIAN_ROOT_DIR-}" RV_NQ_SYSROOT_DIR="${RV_NQ_SYSROOT_DIR-}" RV_DISKS="${RV_DISKS-daily home}" \
		docker compose -f "$S/docker-compose.yml" "$@"
}
say() { echo "=== release: $*"; }

bundle() {
	say "bundle"
	local sys="${NQ_SYSROOT:-$HOME/rvwasm/nativeqt/sysroot}"
	NQ_INFO="${NQ_INFO:-${NQ_INFO_COMMIT:-nativeqt} build of $(date -r "$sys/usr/bin/plasmashell" '+%F %R')}" \
		RV_DEBIAN_ROOT="$W/debian-root-kde" "$S/tools/nq-bundle.sh" "$sys" "$S/tools/x/nq-share" "$W/nq-bundle"
}

root() {
	say "root"
	"$S/tools/build-debian-image.sh" release-root
}

disk() {
	if [ -e "$W/www/disk/release/manifest.json" ] && [ ! "$R/etc/rvwasm/autostart" -nt "$W/www/disk/release/manifest.json" ]; then
		say "disk: up to date"
	else
		say "disk"
		"$S/tools/build-debian-image.sh" disk release 8G
		rm -f "$W/disks/release.ext4"	# (its pieces are what is served)
	fi
	[ -e "$W/www/disk/home/manifest.json" ] || "$S/tools/build-debian-image.sh" home
}

code() {
	say "code (the build container)"
	[ -x "$R/usr/bin/node" ] || { echo "release: no release root at $R (step root)" >&2; exit 1; }
	# The softfloat submodule as files: the container has no way to the Mac's copy a link would point to.
	sf="$S/third_party/berkeley-softfloat-3"
	if [ -L "$sf" ] || [ ! -e "$sf/source/include/softfloat.h" ]; then
		from=${RV_SOFTFLOAT:-${RVWASM_MAIN:-$HOME/dev/rvwasm}/third_party/berkeley-softfloat-3}
		[ -e "$from/source/include/softfloat.h" ] ||
			{ echo "release: no $sf (git submodule update --init, or RV_SOFTFLOAT=DIR)" >&2; exit 1; }
		from=$(realpath "$from")
		rm -rf "$sf" && mkdir -p "$S/third_party" && cp -a "$from" "$sf"
	fi
	mkdir -p "$S/dist"
	RV_DISKS="" RV_DEBIAN_ROOT_DIR="$R" compose --profile build run --rm build toolchain rv gpu stage pack
}

dist() {
	say "dist"
	[ -e "$S/dist/www/dev.html" ] || { echo "release: no dist/www (step code)" >&2; exit 1; }
	mkdir -p "$S/dist/disk"
	for d in release home; do
		[ -e "$W/www/disk/$d/manifest.json" ] || { echo "release: no disk $d (step disk)" >&2; exit 1; }
		rm -rf "$S/dist/disk/$d"
		cp -al "$W/www/disk/$d" "$S/dist/disk/$d" 2> /dev/null || { rm -rf "$S/dist/disk/$d"; cp -a "$W/www/disk/$d" "$S/dist/disk/$d"; }
	done
	rg="${RVGPU_DIR:-$W/www/rvgpu}"
	if [ ! -e "$S/dist/www/rvgpu/rvgpu.wasm" ] && [ -e "$rg/rvgpu.wasm" ] && [ -e "$(dirname "$rg")/rvgpu-worker.js" ]; then
		mkdir -p "$S/dist/www/rvgpu"
		cp "$rg/rvgpu.js" "$rg/rvgpu.wasm" "$S/dist/www/rvgpu/"
		cp "$(dirname "$rg")/rvgpu-worker.js" "$S/dist/www/"
		echo "release: the WebGPU executor from $rg"
	fi
	# This tree's page files and /init (the build's own copy of them can be a release behind): docker/patch-dist.sh.
	"$S/docker/patch-dist.sh" "$S/dist"
	{
		echo "rvwasm release ${RV_RELEASE_ID:-(unnamed)}, built $(date '+%F %R %Z')"
		echo "native plasmashell: $(cat "$R/opt/nq/info.txt" 2> /dev/null)"
		echo "disk release: id $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$S/dist/disk/release/manifest.json")"
		echo "kernel: $(stat -c %s "$S/dist/www/vmlinux-wasm64_nommu.wasm") bytes;" \
			"sound card: $(grep -a -q 'Wasm audio (the browser' "$S/dist/www/vmlinux-wasm64_nommu.wasm" && echo yes || echo NO)"
	} > "$S/dist/RELEASE.txt"
	cat "$S/dist/RELEASE.txt"
	du -shL "$S/dist/www" "$S/dist/disk/"* 2> /dev/null
}

publish() {
	say "publish"
	[ -e "$S/dist/disk" ] || { echo "release: no dist/disk (step dist)" >&2; exit 1; }
	python3 "$S/tools/r2-publish.py" "$S/dist" ${RV_PIECE_BASE:+--base "$RV_PIECE_BASE"}
}

up() {
	say "up"
	compose up -d --build
	compose ps
	echo "open http://localhost:${RV_HTTP_PORT:-8080}/"
}

down() {
	compose down
}

tarball() {
	say "tarball"
	tar -C "$S/dist" -cf - . | gzip -1 > "$S/rvwasm-release.tar.gz.part"
	mv "$S/rvwasm-release.tar.gz.part" "$S/rvwasm-release.tar.gz"
	ls -la "$S/rvwasm-release.tar.gz"
}

[ $# -gt 0 ] || set -- all
for step in "$@"; do
	case "$step" in
	all) bundle; root; disk; code; dist; publish; up ;;
	bundle | root | disk | code | dist | publish | up | down | tarball) "$step" ;;
	*) echo "docker/release.sh: no step $step" >&2; exit 2 ;;
	esac
done
