#!/usr/bin/env python3
"""pgmerge.py OUT DIR...: merge piglit result directories (results.json.bz2) into OUT/results.json.bz2"""
import bz2
import glob
import json
import os
import sys

out, tests = sys.argv[1], {}
meta = None
for d in sys.argv[2:]:
    f = glob.glob(d + '/results.json*')
    if not f:
        print('no results in', d, file=sys.stderr)
        continue
    with (bz2.open(f[0]) if f[0].endswith('.bz2') else open(f[0])) as fh:
        j = json.load(fh)
    meta = meta or j
    tests.update(j['tests'])
meta['tests'] = tests
os.makedirs(out, exist_ok=True)
with bz2.open(out + '/results.json.bz2', 'wt') as fh:
    json.dump(meta, fh)
print(len(tests), 'tests merged')
