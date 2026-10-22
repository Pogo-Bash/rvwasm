#!/bin/bash
# The boot list of the release disk (docker/boot/release.json): a cold boot to the desktop under node-machine
# (tools/release-nm.sh) with the disk worker's trace on, then tools/boot-list.py on what it touched. Run it after the
# release is up (tools/release-setup.sh; the disk is fetched from RELEASE_URL, default http://localhost:8080) and
# commit the file: docker/patch-dist.sh and tools/r2-publish.py put it in the manifest, which is not part of the disk's
# `id`. A list from an older disk is carried over by hash, so this is not needed for every release, only to keep the list
# true to what the desktop reads.
#
#   tools/boot-trace.sh [SECONDS]      (480: the boot reads its last piece some 320 s in when pieces come one at a time;
#                                       one node-machine instance at a time, ~150 s of CPU on the Mac)
set -e
cd "$(dirname "$0")/.."
URL=${RELEASE_URL:-http://localhost:8080}
SECONDS_TO_RUN=${1:-480}
log=$(mktemp -t boot-trace)
trap 'rm -f "$log"' EXIT
echo "boot-trace: a cold boot of $URL for $SECONDS_TO_RUN s (one node-machine; the Mac stays busy meanwhile)"
set +e
DISK_TRACE=1 V=1 WL_DUMP_ACTIONS='screen:1280,800,1,0|windows:1,'$SECONDS_TO_RUN tools/release-nm.sh "sleep $SECONDS_TO_RUN" $(( (SECONDS_TO_RUN + 60) * 1000 )) > "$log" 2>&1
set -e
python3 tools/boot-list.py "$log" --manifest "$URL/disk/release/manifest.json" -o docker/boot/release.json
