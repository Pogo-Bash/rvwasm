#!/usr/bin/env python3
"""closure.py LIBDIR MODULE...: the file names (relative to LIBDIR, symlinks and their targets) the modules need, by NEEDED
(dylink.0), transitively; one per line. Run where the libraries are (the build VM), for rsync --files-from."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from needed import needed

lib = sys.argv[1]
seen = set()

def add(n):
    if n in seen:
        return
    seen.add(n)
    p = os.path.join(lib, n)
    if not os.path.lexists(p):
        sys.exit("missing library " + n)
    if os.path.islink(p):
        add(os.readlink(p))
    else:
        for m in needed(p):
            add(m)

for m in sys.argv[2:]:
    for n in needed(m):
        add(n)
print("\n".join(sorted(seen)))
