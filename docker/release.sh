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
#             (RVGPU_DIR, default ~/rvwasm/www/rvgpu), dist/RELEASE.txt. The executor must be a stamped, clean build
#             (tools/rvgpu-gate.sh; RELEASE.txt records the stamp): an unstamped one stops the release, RV_RVGPU=skip
#             leaves it out
#   nqs       (also the end of dist) the shared-library disk of ?dl=1 (nqs, built by nativeqt/shared/build-nqs-image.sh
#             and tools/split-disk.py into $W/www/disk/nqs) into ./dist/disk, and the library files and manifest
#             (tools/dl-manifest.py --out $W/nqs/dl, or $W/dl; RV_DL_DIR) into ./dist/www/dl, whichever are built, and
#             a line each in RELEASE.txt. Inert without ?dl=1 (nothing asks for them), and the release disk is not
#             touched. Safe to run alone on a dist that is already published: a disk with the same id, and a libs.json
#             with the same keys, are kept as they are (publish may have rewritten them to say where the files are).
#   publish   the disks' pieces to object storage (Cloudflare R2), named by their sha256, and the dist's manifests
#             rewritten to say where (tools/r2-publish.py; docs/deploy.md "Pieces from object storage"). Only the
#             pieces that are not in the bucket yet are uploaded; the manifests are written last. Needs R2_ENDPOINT,
#             R2_BUCKET, R2_ACCESS_KEY_ID and R2_SECRET_ACCESS_KEY in the environment (never in files here:
#             tools/release-setup.sh passes them from the Mac on stdin) and RV_PIECE_BASE (where the page will fetch
#             pieces: https://disk.example.com/disk/); without them this step does nothing and the pieces are served
#             by the site itself. The dist's www/dl libraries go to the bucket too (r2-publish.py --dl, under
#             RV_DL_BASE, default https://disk.plasmaontheweb.com/dl/) and dl/libs.json is pointed at them.
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
	# rvgpu only from a stamped, clean build (tools/rvgpu-gate.sh); the stamp goes into RELEASE.txt. RV_RVGPU=skip: none.
	rvgpu_line="none"
	if [ "${RV_RVGPU-}" = skip ]; then
		rm -rf "$S/dist/www/rvgpu" "$S/dist/www/rvgpu-worker.js"
	elif [ -e "$S/dist/www/rvgpu/rvgpu.wasm" ]; then
		rvgpu_line=$("$S/tools/rvgpu-gate.sh" --line "$S/dist/www/rvgpu/rvgpu.wasm") || {
			echo "release: dist/www/rvgpu/rvgpu.wasm is not a stamped, clean build; refusing (rebuild it with gpu/webgpu/build/wasm.sh" \
				"from a committed tree, or RV_RVGPU=skip to release without the WebGPU executor)" >&2
			exit 1
		}
		rvgpu_line="$rvgpu_line, sha256 $(sha256sum "$S/dist/www/rvgpu/rvgpu.wasm" | cut -c1-16)"
	fi
	# This tree's page files and /init (the build's own copy of them can be a release behind): docker/patch-dist.sh.
	"$S/docker/patch-dist.sh" "$S/dist"
	{
		echo "rvwasm release ${RV_RELEASE_ID:-(unnamed)}, built $(date '+%F %R %Z')"
		echo "native plasmashell: $(cat "$R/opt/nq/info.txt" 2> /dev/null)"
		echo "rvgpu (WebGPU executor): $rvgpu_line"
		echo "disk release: id $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$S/dist/disk/release/manifest.json")"
		echo "kernel: $(stat -c %s "$S/dist/www/vmlinux-wasm64_nommu.wasm") bytes;" \
			"sound card: $(grep -a -q 'Wasm audio (the browser' "$S/dist/www/vmlinux-wasm64_nommu.wasm" && echo yes || echo NO)"
	} > "$S/dist/RELEASE.txt"
	nqs
	cat "$S/dist/RELEASE.txt"
	du -shL "$S/dist/www" "$S/dist/disk/"* 2> /dev/null
}

nqs() {
	say "nqs (the shared libraries of ?dl=1)"
	[ -e "$S/dist/www/dev.html" ] || { echo "release: no dist/www (step code)" >&2; exit 1; }
	mkdir -p "$S/dist/disk"
	disk_id() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$1" 2> /dev/null || true; }
	if [ -e "$W/www/disk/nqs/manifest.json" ]; then
		if [ "$(disk_id "$S/dist/disk/nqs/manifest.json")" != "$(disk_id "$W/www/disk/nqs/manifest.json")" ]; then
			rm -rf "$S/dist/disk/nqs"
			cp -al "$W/www/disk/nqs" "$S/dist/disk/nqs" 2> /dev/null || { rm -rf "$S/dist/disk/nqs"; cp -a "$W/www/disk/nqs" "$S/dist/disk/nqs"; }
			echo "release: disk nqs $(disk_id "$S/dist/disk/nqs/manifest.json") into the dist"
		else
			echo "release: disk nqs: the dist has it"
		fi
	fi
	local dl="${RV_DL_DIR:-$W/nqs/dl}"
	[ -e "$dl/libs.json" ] || dl="$W/dl"
	if [ -e "$dl/libs.json" ]; then
		# (a libs.json that step publish rewrote to the bucket's addresses stays, if it lists the same keys)
		local same
		same=$(python3 -c 'import json,sys
k = lambda p: sorted(l["key"] for l in json.load(open(p))["libs"])
print("yes" if k(sys.argv[1]) == k(sys.argv[2]) else "no")' "$dl/libs.json" "$S/dist/www/dl/libs.json" 2> /dev/null || true)
		if [ "$same" != yes ]; then
			rm -rf "$S/dist/www/dl"
			mkdir -p "$S/dist/www/dl"
			cp -RL "$dl"/. "$S/dist/www/dl/"
			echo "release: dl libraries from $dl into the dist"
		else
			echo "release: dl: the dist has these libraries"
		fi
	fi
	# RELEASE.txt records both (the lines are replaced, not added again)
	if [ -e "$S/dist/RELEASE.txt" ]; then
		grep -v -e '^disk nqs:' -e '^dl:' "$S/dist/RELEASE.txt" > "$S/dist/RELEASE.txt.new" || true
		if [ -e "$S/dist/disk/nqs/manifest.json" ]; then
			echo "disk nqs: id $(disk_id "$S/dist/disk/nqs/manifest.json"), shared libraries for ?dl=1 (mounted at /opt/nq-shared)" >> "$S/dist/RELEASE.txt.new"
		fi
		if [ -e "$S/dist/www/dl/libs.json" ]; then
			python3 -c 'import json,sys; l=json.load(open(sys.argv[1]))["libs"]; print("dl: %d libraries (%d preloaded), %.1f MiB, for ?dl=1" % (len(l), sum(1 for x in l if x.get("preload") is not False), sum(x["size"] for x in l)/1048576))' "$S/dist/www/dl/libs.json" >> "$S/dist/RELEASE.txt.new"
		fi
		mv "$S/dist/RELEASE.txt.new" "$S/dist/RELEASE.txt"
	fi
}

publish() {
	say "publish"
	[ -e "$S/dist/disk" ] || { echo "release: no dist/disk (step dist)" >&2; exit 1; }
	python3 "$S/tools/r2-publish.py" "$S/dist" ${RV_PIECE_BASE:+--base "$RV_PIECE_BASE"}
	[ ! -e "$S/dist/www/dl/libs.json" ] || python3 "$S/tools/r2-publish.py" "$S/dist" --dl
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
	bundle | root | disk | code | dist | nqs | publish | up | down | tarball) "$step" ;;
	*) echo "docker/release.sh: no step $step" >&2; exit 2 ;;
	esac
done
