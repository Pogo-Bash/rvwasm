#!/bin/sh
# The dist, before the page's server starts (docker-compose.yml, service fetch): there already (docker/release.sh
# or tools/pack-www.sh made it in ${RV_DIST:-./dist}), or downloaded from RV_RELEASE_URL, a tar archive of a
# dist (docker/release.sh tarball; gzip or not), and unpacked into it.
set -e
D=/dist
disk=${RV_DISK:-release}
if [ -e $D/www/dev.html ] && { [ -z "$disk" ] || [ -e "$D/disk/$disk/manifest.json" ]; }; then
	echo "fetch: the dist is there ($(cat $D/RELEASE.txt 2> /dev/null | head -1))"
	exit 0
fi
if [ -z "$RV_RELEASE_URL" ]; then
	echo "fetch: no dist in ${RV_DIST:-./dist} (no www/dev.html${disk:+ or disk/$disk}), and no RV_RELEASE_URL to fetch one from." >&2
	echo "fetch: build it (docker/release.sh, README.md \"Run it\"), or set RV_RELEASE_URL to a release archive." >&2
	exit 1
fi
echo "fetch: $RV_RELEASE_URL into ${RV_DIST:-./dist}"
curl -fL --retry 3 "$RV_RELEASE_URL" | { case "$RV_RELEASE_URL" in *.gz | *.tgz) gzip -dc ;; *) cat ;; esac; } | tar -x -C $D
# (Owned by whoever owns the directory on the host, not by the container's root.)
chown -R "$(stat -c %u:%g $D)" $D
[ -e $D/www/dev.html ] || { echo "fetch: the archive had no www/dev.html" >&2; exit 1; }
echo "fetch: done ($(cat $D/RELEASE.txt 2> /dev/null | head -1))"
