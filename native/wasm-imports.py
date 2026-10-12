#!/usr/bin/env python3
"""Print the imports of a Wasm module (module.name and kind), one per line.
llvm-objdump 18 cannot read modules with exception tags; this can."""
import sys
d = open(sys.argv[1], "rb").read()
p = 8
def leb(p):
    r = s = 0
    while True:
        b = d[p]; p += 1; r |= (b & 0x7f) << s; s += 7
        if b < 0x80:
            return r, p
def limits(p):
    fl = d[p]; p += 1
    _, p = leb(p)
    if fl & 1:
        _, p = leb(p)
    return p
kinds = {0: "func", 1: "table", 2: "memory", 3: "global", 4: "tag"}
while p < len(d):
    sid = d[p]; p += 1
    n, p = leb(p); e = p + n
    if sid == 2:
        c, p = leb(p)
        for i in range(c):
            l, p = leb(p); m = d[p:p + l].decode(); p += l
            l, p = leb(p); f = d[p:p + l].decode(); p += l
            k = d[p]; p += 1
            if k == 0: _, p = leb(p)
            elif k == 1: p = limits(p + 1)
            elif k == 2: p = limits(p)
            elif k == 3: p += 2
            elif k == 4: p += 1; _, p = leb(p)
            print(m + "." + f, kinds.get(k, k))
    p = e
