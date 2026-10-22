#!/usr/bin/env python3
"""dl-summary.py LIBS.JSON < log: the DLSTATS line of a run (tests/wasmdyn/dl-hook.js) as a table, library names from the
manifest (tools/dl-manifest.py); modules not in it (the program, trampolines) by size."""
import json
import sys

names = {l["key"]: l["name"] for l in json.load(open(sys.argv[1]))["libs"]}
line = [l for l in sys.stdin.read().splitlines() if l.startswith("DLSTATS ")][-1]
stats = json.loads(line[8:])
print("%-28s %10s %8s %7s %8s" % ("module", "bytes", "compile", "shared", "preload"))
tc = 0
for key, e in sorted(stats.items(), key=lambda kv: -kv[1]["size"]):
    print("%-28s %10d %8d %7d %8d" % (names.get(key, "(%d B)" % e["size"]), e["size"], e["compile"], e["shared"], e["preload"]))
    tc += e["compile"] if key in names else 0
print("libraries compiled by a Worker: %d (preloaded: %d)" % (tc, sum(1 for k in stats if k in names and stats[k]["preload"])))
