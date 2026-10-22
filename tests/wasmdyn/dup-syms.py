#!/usr/bin/env python3
"""dup-syms.py MODULE...: the symbols several modules define (from the linux-wasm.dynsym section tablify.py --dynsym
writes), data symbols first: with the dynamic linker's first-definition-wins binding, which module's copy every
other module gets. Prints counts, and the names matching $PATTERN (default wl_.*_interface)."""
import os
import re
import sys
from collections import defaultdict

def leb(b, p):
    v = s = 0
    while True:
        c = b[p]; p += 1; v |= (c & 0x7f) << s; s += 7
        if not c & 0x80:
            return v, p

def syms(path):
    b = open(path, "rb").read()
    p = 8
    while p < len(b):
        sid = b[p]; p += 1
        n, p = leb(b, p)
        end = p + n
        if sid == 0:
            l, q = leb(b, p)
            if b[q:q + l] == b"linux-wasm.dynsym":
                q += l
                count, q = leb(b, q)
                out = []
                for _ in range(count):
                    l, q = leb(b, q)
                    name = b[q:q + l].decode(); q += l
                    kind = b[q]; q += 1
                    _, q = leb(b, q)
                    out.append((name, kind))
                return out
        p = end
    return []

where = defaultdict(list)
for path in sys.argv[1:]:
    for name, kind in syms(path):
        where[name].append((os.path.basename(path), kind))
dups = {n: w for n, w in where.items() if len(w) > 1}
print("%d symbols defined in more than one module (%d data/global, %d functions)" % (
    len(dups), sum(1 for w in dups.values() if w[0][1] == 3), sum(1 for w in dups.values() if w[0][1] == 0)))
pat = re.compile(os.environ.get("PATTERN", r"wl_.*_interface"))
m = {n: w for n, w in dups.items() if pat.fullmatch(n)}
print("%d matching %s" % (len(m), pat.pattern))
mods = defaultdict(int)
for n, w in m.items():
    for f, _ in w:
        mods[f] += 1
print("  in modules:", dict(mods))
