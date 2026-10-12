#!/bin/bash
# The native plasmashell (WebAssembly, branch nativeqt) as the release image takes it
# (tools/build-debian-image.sh release-root puts OUT in the image as /opt/nq):
#   OUT/plasmashell     the program (nativeqt's build: $SYSROOT/usr/bin/plasmashell)
#   OUT/share/          what the native build installs in share/plasma and share/icons that the Debian root has
#                       not (kdeplasma-addons' widgets and wallpapers, Milou, KScreen's applet, their icons), then
#                       nativeqt's share overrides on top (the default panel without Discover, Milou's popup)
#   OUT/info.txt        which build it is
# The same files for get.sh in development, on the disk instead of fetched at every boot.
#
#   tools/nq-bundle.sh [SYSROOT] [OVERRIDES] [OUT]          (in the build VM or the build container)
#     SYSROOT     default ~/rvwasm/nativeqt/sysroot (nativeqt/run/full-build.sh's)
#     OVERRIDES   default tools/x/nq-share (a copy of nativeqt's nativeqt/share-overrides, fa51f18)
#     OUT         default $RVWASM_WORK/nq-bundle (~/rvwasm/nq-bundle)
# NQ_INFO: the line for info.txt (the nativeqt commit; default: the program's date). RV_DEBIAN_ROOT: the root to
# compare share/ with (default $RVWASM_WORK/debian-root-kde).
set -e
W="${RVWASM_WORK:-$HOME/rvwasm}"
SYS="${1:-$HOME/rvwasm/nativeqt/sysroot}"
OV="${2:-$(realpath "$(dirname "$0")")/x/nq-share}"
OUT="${3:-$W/nq-bundle}"
DEB="${RV_DEBIAN_ROOT:-$W/debian-root-kde}"
P="$SYS/usr/bin/plasmashell"
[ -f "$P" ] || { echo "nq-bundle: no $P (nativeqt's build)" >&2; exit 1; }
[ -d "$OV/share" ] || { echo "nq-bundle: no $OV/share (nativeqt's share overrides)" >&2; exit 1; }
[ -d "$DEB/usr/share" ] || { echo "nq-bundle: no Debian root at $DEB" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT/share"
install -m 755 "$P" "$OUT/plasmashell"
list=$(mktemp)
(cd "$SYS/usr" && find share/plasma share/icons -type f | while read -r f; do [ -e "$DEB/usr/$f" ] || echo "$f"; done) > "$list"
tar -C "$SYS/usr" -cf - -T "$list" | tar -C "$OUT" -xf -
cp -r "$OV/share/." "$OUT/share/"
echo "${NQ_INFO:-nativeqt build of $(date -r "$P" '+%F %R')}" > "$OUT/info.txt"
echo "nq-bundle: $OUT: plasmashell $(( $(stat -c %s "$P") / 1048576 )) MiB, share $(wc -l < "$list") files" \
	"+ $(find "$OV/share" -type f | wc -l) overrides; $(cat "$OUT/info.txt")"
rm -f "$list"
