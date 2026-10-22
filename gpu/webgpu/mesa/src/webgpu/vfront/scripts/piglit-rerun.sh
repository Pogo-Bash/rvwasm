#!/bin/bash
# (In the VM, outside the chroot) re-run what crashed or timed out in a piglit-wgvk.sh run: a test running when
# the server dies (the leak watchdog, an assertion in another test) is reported as a crash with "lost connection
# to rendering server" though it is fine. The tests are run again against a fresh server, 2 at a time, and their
# new results replace the old ones in NAME (the first run's kept as NAME-first; run it again for what is left).   piglit-rerun.sh NAME [JOBS]
S=$(cd "$(dirname "$0")" && pwd); T=${S/$HOME\/rvwasm\/gpu-t6/\/t6}
name=${1:-gl-wgvk}; j=${2:-2}
P=$HOME/rvwasm/gpu-t6/piglit
python3 - $P/$name $P/$name-rr.txt <<'PY'
import bz2, json, sys
d = json.load(bz2.open(sys.argv[1] + '/results.json.bz2'))['tests']
names = sorted(k for k, v in d.items() if v['result'] in ('crash', 'timeout') and '#' not in k)
open(sys.argv[2], 'w').write('\n'.join(names) + '\n')
print(len(names), 'to re-run')
PY
rm -rf $P/$name-rr
bash $S/main-run.sh --vk wgvk pg-$name-rr "PIGLIT_TIMEOUT_S=${PIGLIT_TIMEOUT_S:-7200} SERVER=external TESTLIST=/t6/piglit/$name-rr.txt bash $T/piglit-run.sh $name-rr quick x $j" 2>&1 | grep -E "piglit rc|main-run|total|crash"
[ -d $P/$name-first ] || cp -r $P/$name $P/$name-first
python3 $S/../tools/pgmerge.py $P/$name.new $P/$name $P/$name-rr && rm -rf $P/$name && mv $P/$name.new $P/$name
python3 $S/../tools/pgcmp.py $P/gl-vfront $P/$name 40
