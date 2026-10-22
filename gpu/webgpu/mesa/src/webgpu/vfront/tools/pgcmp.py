#!/usr/bin/env python3
"""pgcmp.py RESULTS_A RESULTS_B: compare two piglit result directories (results.json.bz2): counts per status
and the tests whose status differs, grouped by the pair of statuses."""
import bz2
import collections
import glob
import json
import sys


def load(d):
    f = glob.glob(d + '/results.json*')[0]
    with (bz2.open(f) if f.endswith('.bz2') else open(f)) as fh:
        return json.load(fh)['tests']


a, b = load(sys.argv[1]), load(sys.argv[2])
ca = collections.Counter(v['result'] for v in a.values())
cb = collections.Counter(v['result'] for v in b.values())
print('%-12s %8s %8s' % ('status', sys.argv[1].rstrip('/').split('/')[-1], sys.argv[2].rstrip('/').split('/')[-1]))
for st in sorted(set(ca) | set(cb)):
    print('%-12s %8d %8d' % (st, ca[st], cb[st]))
diff = collections.defaultdict(list)
for k in sorted(set(a) | set(b)):
    ra = a.get(k, {}).get('result', '-')
    rb = b.get(k, {}).get('result', '-')
    if ra != rb:
        diff[(ra, rb)].append(k)
for (ra, rb), ks in sorted(diff.items(), key=lambda x: -len(x[1])):
    print('%s -> %s: %d' % (ra, rb, len(ks)))
    for k in ks[:int(sys.argv[3]) if len(sys.argv) > 3 else 15]:
        print('   ', k)
