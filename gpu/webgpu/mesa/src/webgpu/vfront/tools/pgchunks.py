#!/usr/bin/env python3
"""pgchunks.py RESULTS MAXTESTS OUTPREFIX: split the tests of a piglit result directory into chunks of at most
MAXTESTS (in name order, so a chunk is a few neighbouring groups) and write each as a piglit --test-list file
OUTPREFIX1.txt, OUTPREFIX2.txt, ...; prints the number of chunks. A server is restarted between chunks, so what
a long run leaks (the server's memory grew past 10 GB in 1760 tests) stays bounded."""
import bz2
import glob
import json
import re
import sys


def load(d):
    f = glob.glob(d + '/results.json*')[0]
    with (bz2.open(f) if f.endswith('.bz2') else open(f)) as fh:
        return sorted(json.load(fh)['tests'])


names = load(sys.argv[1])
limit = int(sys.argv[2])
# piglit's --test-list drops everything after a '#': those names go to OUTPREFIXhash.txt, one -t regex
hashed = [n for n in names if '#' in n]
names = [n for n in names if '#' not in n]
with open(sys.argv[3] + 'hash.txt', 'w') as f:
    f.write('^(' + '|'.join(re.escape(n) for n in hashed) + ')$\n' if hashed else '')
chunks = [names[i:i + limit] for i in range(0, len(names), limit)]
for i, ch in enumerate(chunks, 1):
    with open('%s%d.txt' % (sys.argv[3], i), 'w') as f:
        f.write('\n'.join(ch) + '\n')
print(len(chunks))
