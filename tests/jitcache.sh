#!/bin/bash
# What would a cache of translated code be worth? Run inside Debian on the
# machine (needs gcc and g++):
#
#   cp /rv/jitcache.sh /debian/root/; debian "bash /root/jitcache.sh"
#
# 1. How long some work takes with the translator waiting for code to be hot
#    (as it does), and with it translating at first touch: the difference is
#    what knowing beforehand which code is hot could save, at most. (At
#    first touch the regions are made without knowing which blocks belong
#    together, so they are small: a list that also said that would do better.)
# 2. A log of every region translated by every process (RV_JIT_LOG), and from
#    it how often the same region of the same file is translated again: in
#    any process, and where the file is loaded at the same address.

cd /root || exit 1
printf '#include <iostream>\n#include <map>\n#include <string>\nint main() { std::map<std::string, int> m{{"c++", 20}}; std::cout << "c++ " << m.size() << "\\n"; }\n' > jc.cc
printf '#include <stdio.h>\nint main(void) { printf("c\\n"); return 0; }\n' > jc.c

work_cxx() { g++ -O2 -std=c++20 jc.cc -o jc-cxx; }
work_c() { for i in 1 2 3 4 5; do gcc -O2 jc.c -o jc-c; done; }
work_small() { for i in $(seq 1 60); do ls /usr/bin > /dev/null; grep -c root /etc/passwd > /dev/null; done; }
work_firefox() { rm -rf /tmp/jc-profile; mkdir -p /tmp/jc-profile; timeout 300 firefox-esr --headless --profile /tmp/jc-profile --screenshot /tmp/jc.png about:blank; }
work_python() { for i in 1 2 3; do python3 -c 'import json, hashlib, sqlite3, re; print(len(json.dumps(list(range(20000)))))' > /dev/null; done; }

timed() {
	local s e
	s=$(date +%s%N)
	"$@" > /dev/null 2>&1
	e=$(date +%s%N)
	echo $(( (e - s) / 1000000 ))
}

# Everything once, so that the files are in memory.
WORK="cxx c small python"
work_cxx; work_c; work_small; work_python
if [ -x /usr/bin/firefox-esr ]; then
	work_firefox > /tmp/jc-firefox.log 2>&1
	if [ -s /tmp/jc.png ]; then
		WORK="$WORK firefox"
	else
		echo "TIME (firefox left out: no screenshot. $(tail -n 1 /tmp/jc-firefox.log | cut -c1-120))"
	fi
fi

echo "TIME work hot=200,start=100000 hot=20,start=10000 hot=1,start=0 hot=1,start=0,warm=0 (ms, the better of two)"
for w in $WORK; do
	line="TIME $w"
	for conf in "RV_JIT_HOT=200 RV_JIT_START=100000" "RV_JIT_HOT=20 RV_JIT_START=10000" "RV_JIT_HOT=1 RV_JIT_START=0" "RV_JIT_HOT=1 RV_JIT_START=0 RV_JIT_WARM=0"; do
		a=$(env $conf bash -c "$(declare -f work_$w timed); timed work_$w")
		b=$(env $conf bash -c "$(declare -f work_$w timed); timed work_$w")
		line="$line $(( a < b ? a : b ))"
	done
	echo "$line"
done

rm -f /tmp/jit.log
export RV_JIT_LOG=/tmp/jit.log
work_cxx; work_c; work_small; work_python
work_cxx
unset RV_JIT_LOG
if [[ "$WORK" == *firefox* ]]; then
	RV_JIT_LOG=/tmp/jit-firefox.log work_firefox > /dev/null 2>&1
	RV_JIT_LOG=/tmp/jit-firefox.log work_firefox > /dev/null 2>&1
fi
python3 - <<'END'
import collections
import os
def read(path):
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path, errors="replace"):
        p = line.split()
        if len(p) >= 12 and p[0] == "R":
            rows.append(dict(pid=p[1], file=p[2], off=p[3], base=p[4], insns=int(p[5]), blocks=int(p[6]), hash=p[7],
                             fixed=p[8] == "1", made=int(p[9]), compiled=int(p[10]), size=int(p[11])))
    return rows
rows = read("/tmp/jit.log")
def report(name, rows):
    n = len(rows)
    if not n:
        return
    ns = sum(r["made"] + r["compiled"] for r in rows)
    print("LOG %s: %d regions by %d processes, %d instructions, %d KiB of wasm; %d ms making them, %d ms the host compiling" % (
        name, n, len({r["pid"] for r in rows}), sum(r["insns"] for r in rows), sum(r["size"] for r in rows) // 1024,
        sum(r["made"] for r in rows) // 1000000, sum(r["compiled"] for r in rows) // 1000000))
    fixed = [r for r in rows if r["fixed"]]
    print("LOG   in pages that are a file's own: %d%% of regions, %d%% of the time; in no file (made at run time): %d%%" % (
        100 * len(fixed) // n, 100 * sum(r["made"] + r["compiled"] for r in fixed) // max(ns, 1),
        100 * sum(1 for r in rows if r["file"] == "0:0") // n))
    for label, key in (("same file, place and code", lambda r: (r["file"], r["off"], r["hash"])),
                       ("... and loaded at the same address", lambda r: (r["file"], r["off"], r["hash"], r["base"])),
                       ("same file and place, whatever the region's shape", lambda r: (r["file"], r["off"]))):
        seen = set(); again = 0; again_ns = 0
        for r in fixed:
            k = key(r)
            if k in seen:
                again += 1; again_ns += r["made"] + r["compiled"]
            seen.add(k)
        print("LOG   seen before (%s): %d%% of all regions, %d%% of all the time; %d distinct" % (
            label, 100 * again // n, 100 * again_ns // max(ns, 1), len(seen)))
report("compilers, small programs, python", rows)
report("firefox, started twice", read("/tmp/jit-firefox.log"))
rows += read("/tmp/jit-firefox.log")
files = collections.Counter(r["file"] for r in rows)
print("LOG files with most regions:", ", ".join("%s x%d" % kv for kv in files.most_common(6)))
bases = collections.defaultdict(set)
for r in rows:
    if r["fixed"]:
        bases[r["file"]].add(r["base"])
print("LOG files loaded at more than one address: %d of %d; the most: %s" % (
    sum(1 for b in bases.values() if len(b) > 1), len(bases),
    ", ".join("%s at %d" % (f, len(b)) for f, b in sorted(bases.items(), key=lambda kv: -len(kv[1]))[:4])))
END
