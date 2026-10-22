#!/usr/bin/env python3
"""check-dylib.py MODULE [--provider FILE]... : is MODULE a valid dylink.0 side module that the wasmdyn
prototype's tools (tablify.py --dynsym, ldso.c) accept, and does everything it imports have a provider?

Checks: dylink.0 first with MEM_INFO; tablify's subsection 0x80 present and consistent with the table
import and the dylink table size; a "linux-wasm.dynsym" section; only tags/entry points left as wasm
exports; every function import (env.NAME) and GOT.func/GOT.mem import is exported by a provider (the C
library ld-wasm, other libraries) or by MODULE itself (a weak definition that imports and exports itself:
ld.so binds those to trampolines); tag imports are the C library's. Prints the counts.
"""
import sys
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from tablify import leb, name


def sections(b):
    p = 8
    out = []
    while p < len(b):
        sid = b[p]
        n, q = leb(b, p + 1)
        out.append((sid, b[q:q + n]))
        p = q + n
    return out


def parse(path):
    b = open(path, "rb").read()
    assert b[:8] == b"\0asm\1\0\0\0", "not a wasm module"
    secs = sections(b)
    r = {"size": len(b), "imports": [], "exports": [], "dynsym": [], "subs": {}, "needed": []}
    assert secs[0][0] == 0 and name(secs[0][1], 0)[0] == b"dylink.0", "dylink.0 is not the first section"
    d = secs[0][1]
    _, p = name(d, 0)
    while p < len(d):
        sid = d[p]
        n, q = leb(d, p + 1)
        r["subs"][sid] = d[q:q + n]
        p = q + n
    for sid, s in secs:
        if sid == 2:
            c, p = leb(s, 0)
            for _ in range(c):
                m, p = name(s, p)
                f, p = name(s, p)
                k = s[p]
                p += 1
                if k == 0:
                    _, p = leb(s, p)
                elif k == 1:
                    p += 1
                    fl = s[p]
                    p += 1
                    v, p = leb(s, p)
                    r["table_min"] = v
                    if fl & 1:
                        _, p = leb(s, p)
                elif k == 2:
                    fl = s[p]
                    p += 1
                    _, p = leb(s, p)
                    if fl & 1:
                        _, p = leb(s, p)
                elif k == 3:
                    p += 2
                elif k == 4:
                    p += 1
                    _, p = leb(s, p)
                r["imports"].append((m.decode(), f.decode(), k))
        elif sid in (7,) or (sid == 0 and name(s, 0)[0] == b"linux-wasm.dynsym"):
            if sid == 0:
                _, q = name(s, 0)
                s = s[q:]
            c, p = leb(s, 0)
            lst = r["dynsym"] if sid == 0 else r["exports"]
            for _ in range(c):
                nm, p = name(s, p)
                k = s[p]
                _, p = leb(s, p + 1)
                lst.append((nm.decode(), k))
    mem = r["subs"][1]
    r["mem_size"], p = leb(mem, 0)
    r["mem_align"], p = leb(mem, p)
    r["tab_size"], p = leb(mem, p)
    return r


def main():
    args = sys.argv[1:]
    providers = []
    while "--provider" in args:
        i = args.index("--provider")
        providers.append(args[i + 1])
        del args[i:i + 2]
    m = parse(args[0])
    prov = set()
    for pp in providers:
        q = parse(pp)
        prov |= {n for n, k in (q["dynsym"] or q["exports"])}
    own = {n for n, k in m["dynsym"]}
    fails = []
    if 0x80 not in m["subs"]:
        fails.append("tablify subsection 0x80 missing")
    else:
        old, _ = leb(m["subs"][0x80], 0)
        nfun = sum(1 for n, k in m["dynsym"] if k == 0)
        if old + nfun != m["tab_size"]:
            fails.append("table size %d != old %d + %d function exports" % (m["tab_size"], old, nfun))
        if m.get("table_min", 0) != m["tab_size"] and m.get("table_min", 0) < m["tab_size"]:
            fails.append("table import min %s < dylink table size %d" % (m.get("table_min"), m["tab_size"]))
    if not m["dynsym"]:
        fails.append("no linux-wasm.dynsym section")
    fi = [f for mod, f, k in m["imports"] if mod == "env" and k == 0]
    gm = [f for mod, f, k in m["imports"] if mod == "GOT.mem"]
    gf = [f for mod, f, k in m["imports"] if mod == "GOT.func"]
    tags = [f for mod, f, k in m["imports"] if k == 4]
    host = {"memory", "__indirect_function_table", "__stack_pointer", "__memory_base", "__table_base", "__table_base32"}
    self_fi = [f for f in fi if f in own]
    self_gm = [f for f in gm if f in own]
    self_gf = [f for f in gf if f in own]
    miss_fi = sorted(f for f in set(fi) if f not in own and f not in prov and f not in host and not f.startswith("__wasm_syscall") and f != "__wasm_dl")
    miss_gm = sorted(f for f in set(gm) if f not in own and f not in prov)
    miss_gf = sorted(f for f in set(gf) if f not in own and f not in prov)
    nfun = sum(1 for n, k in m["dynsym"] if k == 0)
    ndata = sum(1 for n, k in m["dynsym"] if k == 3)
    print("%s: %d bytes; dylink: mem %d (align 2^%d), table %d" % (args[0], m["size"], m["mem_size"], m["mem_align"], m["tab_size"]))
    needed = []
    if 2 in m["subs"]:
        c, p = leb(m["subs"][2], 0)
        for _ in range(c):
            nm, p = name(m["subs"][2], p)
            needed.append(nm.decode())
    print("  needed: %s" % needed)
    print("  exports: %d in dynsym (%d functions, %d data/globals, %d other); %d wasm exports left %s" % (
        len(m["dynsym"]), nfun, ndata, len(m["dynsym"]) - nfun - ndata, len(m["exports"]), [n for n, k in m["exports"]]))
    print("  imports: %d total; %d env functions (%d are its own weak definitions -> trampolines), %d GOT.mem (%d own), %d GOT.func (%d own), tags %s" % (
        len(m["imports"]), len(fi), len(self_fi), len(gm), len(self_gm), len(gf), len(self_gf), tags))
    if providers:
        for what, lst in (("function", miss_fi), ("GOT.mem", miss_gm), ("GOT.func", miss_gf)):
            print("  unresolved %s imports: %d%s" % (what, len(lst), (" " + " ".join(lst[:40])) if lst else ""))
    for f in fails:
        print("  FAIL:", f)
    sys.exit(1 if fails else 0)


main()
