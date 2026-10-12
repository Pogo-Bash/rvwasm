#!/bin/bash
# Put Blender (blenderweb/: Puter's WebGPU build, shown as a window of the desktop) on the public server, to be served
# by Caddy at /blender/ (docker/Caddyfile; docs/deploy.md "Blender"). From the Mac.
#
#   tools/deploy-blender.sh [-n] [SERVER]      SERVER: an ssh name (or $RV_SERVER); -n: list, copy nothing
#
# On the server, ~/rvwasm/blender-web/ (mounted into Caddy at /blender):
#   index.html, main.js          this tree's blenderweb/www/ (compared by content)
#   release/                     Puter's release 1.0.0, downloaded ON THE SERVER by blenderweb/fetch-release.sh (checked by
#                                its SHA-256) the first time, then compressed for the browser: blender.wasm.zst/.gz and
#                                assets.tar.zst/.gz next to the files (zstd with an 8 MB window at most, the limit browsers
#                                accept for Content-Encoding: zstd; gzip -9 for the rest), so that Caddy sends them as they are.
# Nothing here changes the page itself: the page is told where Blender is by RV_BLENDER_URL in the server's .env (rv-site.json).
set -e
WT=$(cd "$(dirname "$0")/.." && pwd)
DRY=
[ "$1" != -n ] || { DRY=-n; shift; }
SERVER=${1:-${RV_SERVER:?name the server (an ssh name) as the first argument or in RV_SERVER}}
DEST=${RV_BLENDER_DEST:-rvwasm/blender-web}
say() { echo "=== deploy-blender: $*"; }

say "the page (blenderweb/www/) and the fetch script"
ssh "$SERVER" "mkdir -p $DEST/release rvwasm/blenderweb"
rsync -ai --checksum ${DRY:+-n} "$WT/blenderweb/www/" "$SERVER:$DEST/"
rsync -ai --checksum ${DRY:+-n} "$WT/blenderweb/fetch-release.sh" "$SERVER:rvwasm/blenderweb/"

if [ -n "$DRY" ]; then
	ssh "$SERVER" "ls -la $DEST/release 2> /dev/null || true"
	say "dry run: nothing else done"
	exit 0
fi

say "the release (downloaded on the server if it is not there), compressed"
ssh "$SERVER" "set -e; cd rvwasm
	R=\$HOME/$DEST/release
	if [ ! -e \$R/blender.wasm ] || [ ! -e \$R/SOURCE.txt ]; then
		blenderweb/fetch-release.sh \$R
	fi
	for f in blender.wasm assets.tar; do
		# (nice: the server's two cores are also the site's)
		if [ ! -e \$R/\$f.zst ] || [ \$R/\$f -nt \$R/\$f.zst ]; then nice zstd -q -f -15 --zstd=wlog=23 \$R/\$f -o \$R/\$f.zst.tmp && mv \$R/\$f.zst.tmp \$R/\$f.zst; fi
		if [ ! -e \$R/\$f.gz ] || [ \$R/\$f -nt \$R/\$f.gz ]; then nice gzip -9 -c \$R/\$f > \$R/\$f.gz.tmp && mv \$R/\$f.gz.tmp \$R/\$f.gz; fi
	done
	ls -la \$R"
say "done"
