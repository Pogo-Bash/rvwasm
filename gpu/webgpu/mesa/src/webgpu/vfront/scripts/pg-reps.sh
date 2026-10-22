#!/bin/bash
# (In the VM) representatives of a failure group: for each piglit test NAME (a result key) of the file
# LIST (one per line), the command piglit ran for it (from RESULTS), run against a fresh wgvk server (main-run.sh),
# printing the test's status line and the distinct wgvk/vfront lines the server logged.
#   VFRONT_BUILD=vk-03 pg-reps.sh RESULTS_DIR LIST
S=$(cd "$(dirname "$0")" && pwd)
python3 - "$1" "$2" > /tmp/pg-reps.$$ <<'PY'
import bz2, glob, json, sys
d = json.load(bz2.open(glob.glob(sys.argv[1] + '/results.json*')[0]))['tests']
for n in open(sys.argv[2]):
    n = n.strip()
    if n in d:
        print(n + '\t' + d[n]['command'])
PY
while IFS=$'\t' read -r name cmd; do
	echo "### $name"
	out=$(RESTART=0 bash $S/main-run.sh --vk wgvk rep "SERVER=external T=60 LINES_OUT=3 SLINES=0 bash /t6/$(basename $(dirname $S))/scripts/pg-one.sh '$cmd'" 2>&1 | grep -v "artificially\|^main-run")
	echo "$out" | cut -c1-160 | tail -4
	grep -E "wgvk|vfront" $HOME/rvwasm/gpu-t6/log/rep-server.log | grep -v "artificially\|faked for" | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -4 | cut -c1-230
done < /tmp/pg-reps.$$
rm -f /tmp/pg-reps.$$
