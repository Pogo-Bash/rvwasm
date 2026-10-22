#!/usr/bin/env python3
"""needed-closure.py PROGRAM [LIBDIR]: the shared libraries PROGRAM needs, transitively (dylink.0 NEEDED), with sizes.
Run in the build VM against sysroot-shared/usr/lib. Plugins are not NEEDED; list those separately."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.argv, args = sys.argv[:1], sys.argv[1:]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "check-dylib.py")).read().split("def main")[0])
prog = args[0]
libdir = args[1] if len(args) > 1 else os.path.expanduser("~/rvwasm/nativeqt/sysroot-shared/usr/lib")
seen, order = {}, []
def needed(path):
    r = parse(path)
    out = []
    if 2 in r["subs"]:
        c, p = leb(r["subs"][2], 0)
        for _ in range(c):
            nm, p = name(r["subs"][2], p)
            out.append(nm.decode())
    return out
def walk(path):
    for n in needed(path):
        real = os.path.realpath(os.path.join(libdir, n))
        if real in seen:
            continue
        seen[real] = n
        order.append(real)
        walk(real)
walk(prog)
tot = os.path.getsize(prog)
for r in order:
    sz = os.path.getsize(r)
    tot += sz
    print("%10d  %s   (needed as %s)" % (sz, os.path.basename(r), seen[r]))
print("%10d  total with %s (%d bytes)" % (tot, os.path.basename(prog), os.path.getsize(prog)))
