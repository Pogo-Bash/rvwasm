#!/bin/bash
# (In the VM, outside the chroot) the GL 1.x-3.3 piglit selection (piglit-gl.sh's) through vfront-vtest on
# wgvk on Dawn. The selection is cut into chunks of CHUNK (default 600) tests by name prefix (tools/pgchunks.py,
# from the names of the zink-on-lavapipe run), each chunk against its own fresh server (main-run.sh): the
# server's memory grows past 10 GB in 1760 tests on wgvk (the VM has 12), so a server runs one chunk only, and
# main-run.sh restarts one that goes past MAXRSS_MB. Results: ~/rvwasm/gpu-t6/piglit/NAME-cN, merged into NAME
# and compared with the zink-on-lavapipe run (gl-vfront).
#   VFRONT_BUILD=vk-03 piglit-wgvk.sh NAME [JOBS]     (VFRONT_CAPS=honest: no faked caps; CHUNKS="1 3": only those)
S=$(cd "$(dirname "$0")" && pwd); T=${S/$HOME\/rvwasm\/gpu-t6/\/t6}
name=${1:-gl-wgvk}; j=${2:-4}
P=$HOME/rvwasm/gpu-t6/piglit
n=$(python3 $S/../tools/pgchunks.py $P/gl-vfront ${CHUNK:-600} $P/$name-chunk-)
echo "piglit-wgvk: $n chunks"
i=0
for i in $(seq $n); do
	[ -n "$CHUNKS" ] && ! echo " $CHUNKS " | grep -q " $i " && continue
	rm -rf $P/$name-c$i
	echo "=== chunk $i/$n $(date +%T)"
	bash $S/main-run.sh --vk wgvk pg-$name-c$i "PIGLIT_TIMEOUT_S=${PIGLIT_TIMEOUT_S:-7200} SERVER=external TESTLIST=/t6/piglit/$name-chunk-$i.txt bash $T/piglit-run.sh $name-c$i quick 'x' $j" 2>&1 | grep -E "piglit rc|main-run|pass|fail|crash|skip|timeout|total"
done
# (the names with a '#' in them, which --test-list cannot carry: one -t regex)
if [ -s $P/$name-chunk-hash.txt ] && { [ -z "$CHUNKS" ] || echo " $CHUNKS " | grep -q " h "; }; then
	rm -rf $P/$name-ch
	echo "=== chunk h $(date +%T)"
	re=$(cat $P/$name-chunk-hash.txt)
	bash $S/main-run.sh --vk wgvk pg-$name-ch "PIGLIT_TIMEOUT_S=${PIGLIT_TIMEOUT_S:-7200} SERVER=external bash $T/piglit-run.sh $name-ch quick '$re' $j" 2>&1 | grep -E "piglit rc|main-run|total"
fi
rm -rf $P/$name
python3 $S/../tools/pgmerge.py $P/$name $(for k in $(seq $n) h; do echo $P/$name-c$k; done)
python3 $S/../tools/pgcmp.py $P/gl-vfront $P/$name 40
