#!/usr/bin/env python3
"""tablify.py MODULE: give every exported function of a position independent module (wasm-ld -pie or
-shared) a slot in the function table, put there by the module's own element segment.

The dynamic linker needs a table slot for every function another module may take the address of
(GOT.func imports) and for every function dlsym() may return. Without this, ld-wasm has the host put
the exports into the table one by one from JavaScript, in every thread, which costs a JavaScript
wrapper and a table.set per function per thread (libbig.so's 20000: 15 ms per thread start). With
it, instantiation fills the slots natively.

What it changes: the active element segment that starts at __table_base gets the module's function
exports appended (in export order, as the dynamic linker numbers them); dylink.0's table size grows by
as many; the table import's minimum too (the runtime sizes the program's table by it); and dylink.0
gets a subsection 0x80 (ours, not in the tool conventions) holding the old table size, which tells the
dynamic linker that the k-th function export is in slot __table_base + old size + k.

With --dynsym, the module's exports are moved into a custom section, "linux-wasm.dynsym" (the export
section's own encoding), and only what the host itself needs stays exported: tags, and the runtime's
entry points. V8 makes a JavaScript function for every exported function of every instance, in every
Worker: 20000 exports cost 9 ms per instantiation, the same functions in the table and not exported
0.9 ms (main5 with libbig.so and libbig-noexp.so). The dynamic linker then reads the symbols from
the custom section and binds function imports to table slots.

A linker could do this itself (an option to wasm-ld); this is the same thing after the fact.
"""
KEEP = {b"_start", b"__set_tls_base", b"__libc_clone_callback", b"__wasm_apply_data_relocs",
        b"__libc_handle_signal", b"__dl_clone_hook"}
import sys


def leb(b, p):
    v = shift = 0
    while True:
        c = b[p]
        p += 1
        v |= (c & 0x7f) << shift
        shift += 7
        if not c & 0x80:
            return v, p


def sleb(b, p):
    v = shift = 0
    while True:
        c = b[p]
        p += 1
        v |= (c & 0x7f) << shift
        shift += 7
        if not c & 0x80:
            if c & 0x40:
                v -= 1 << shift
            return v, p


def enc(v):
    out = bytearray()
    while True:
        c = v & 0x7f
        v >>= 7
        out.append(c | (0x80 if v else 0))
        if not v:
            return bytes(out)


def name(b, p):
    n, p = leb(b, p)
    return b[p:p + n], p + n


def expr(b, p):
    start = p
    while True:
        op = b[p]
        p += 1
        if op == 0x0b:
            return b[start:p], p
        if op in (0x41, 0x42):
            _, p = sleb(b, p)
        elif op in (0x23, 0xd2):
            _, p = leb(b, p)
        elif op == 0xd0:
            p += 1
        elif op in (0x6a, 0x6b, 0x6c, 0x7c, 0x7d, 0x7e):
            pass
        else:
            raise ValueError("constant expression opcode %#x" % op)


def section(sid, body):
    return bytes([sid]) + enc(len(body)) + body


def main():
    dynsym = "--dynsym" in sys.argv[1:]
    path = [a for a in sys.argv[1:] if not a.startswith("--")][0]
    b = open(path, "rb").read()
    assert b[:8] == b"\0asm\1\0\0\0"
    sections = []
    p = 8
    while p < len(b):
        sid = b[p]
        n, q = leb(b, p + 1)
        sections.append([sid, b[q:q + n]])
        p = q + n

    def find(sid, custom=None):
        for s in sections:
            if s[0] == sid and (custom is None or name(s[1], 0)[0] == custom):
                return s
        return None

    dylink = find(0, b"dylink.0")
    if not dylink:
        sys.exit(path + ": no dylink.0 section")
    _, body_start = name(dylink[1], 0)
    subs = []
    p = body_start
    d = dylink[1]
    while p < len(d):
        sid = d[p]
        n, q = leb(d, p + 1)
        subs.append([sid, d[q:q + n]])
        p = q + n
    if any(s[0] == 0x80 for s in subs):
        print(path + ": already done")
        return

    # Function exports, in export order.
    funcs = []
    exp = find(7)
    if exp:
        e = exp[1]
        count, p = leb(e, 0)
        for _ in range(count):
            _, p = name(e, p)
            kind = e[p]
            idx, p = leb(e, p + 1)
            if kind == 0:
                funcs.append(idx)

    # Imports: the table import's minimum, and the global index of __table_base32 / __table_base.
    imp = find(2)
    table_base_global = None
    new_imports = bytearray()
    table_limits_at = None
    entries = []
    if imp:
        m = imp[1]
        count, p = leb(m, 0)
        gidx = 0
        for _ in range(count):
            start = p
            mod, p = name(m, p)
            fld, p = name(m, p)
            kind = m[p]
            p += 1
            if kind == 0:
                _, p = leb(m, p)
            elif kind == 1:
                p += 1  # reftype
                flags = m[p]
                lim = p
                p += 1
                _, p = leb(m, p)
                if flags & 1:
                    _, p = leb(m, p)
                entries.append(("table", m[start:lim], flags, m[lim + 1:p]))
                continue
            elif kind == 2:
                flags = m[p]
                p += 1
                _, p = leb(m, p)
                if flags & 1:
                    _, p = leb(m, p)
            elif kind == 3:
                if mod == b"env" and fld in (b"__table_base32", b"__table_base") and (
                        table_base_global is None or fld == b"__table_base32"):
                    table_base_global = gidx
                gidx += 1
                p += 2
            elif kind == 4:
                p += 1
                _, p = leb(m, p)
            entries.append(("raw", m[start:p]))

    # dylink.0 MEM_INFO
    mem = next(s for s in subs if s[0] == 1)
    msize, p = leb(mem[1], 0)
    malign, p = leb(mem[1], p)
    tsize, p = leb(mem[1], p)
    talign, p = leb(mem[1], p)
    new_tsize = tsize + len(funcs)
    mem[1] = enc(msize) + enc(malign) + enc(new_tsize) + enc(talign)
    subs.append([0x80, enc(tsize)])

    # The element segment at __table_base (wasm-ld makes one active segment for table 0).
    elem = find(9)
    if elem:
        e = elem[1]
        count, p = leb(e, 0)
        segs = []
        done = False
        for _ in range(count):
            start = p
            flags, p = leb(e, p)
            if flags == 0 and not done:
                off, p = expr(e, p)
                n, p = leb(e, p)
                idxs = []
                for _ in range(n):
                    i, p = leb(e, p)
                    idxs.append(i)
                if n != tsize:
                    sys.exit(path + ": the element segment has %d entries, the table size is %d" % (n, tsize))
                idxs += funcs
                segs.append(enc(0) + off + enc(len(idxs)) + b"".join(enc(i) for i in idxs))
                done = True
                continue
            # Other segments: skip them as they are.
            if flags in (0, 2, 4, 6):
                if flags in (2, 6):
                    _, p = leb(e, p)
                _, p = expr(e, p)
            if flags in (1, 2, 3, 5, 6, 7):
                p += 1  # elemkind / reftype
            n, p = leb(e, p)
            for _ in range(n):
                if flags < 4:
                    _, p = leb(e, p)
                else:
                    _, p = expr(e, p)
            segs.append(e[start:p])
        if not done:
            sys.exit(path + ": no active element segment for table 0")
        elem[1] = enc(len(segs)) + b"".join(segs)
    elif funcs:
        if tsize:
            sys.exit(path + ": a table size but no element section")
        if table_base_global is None:
            sys.exit(path + ": no __table_base import")
        seg = enc(0) + bytes([0x23]) + enc(table_base_global) + b"\x0b" + enc(len(funcs)) + b"".join(enc(i) for i in funcs)
        # The element section goes after the start section (8), or else after exports (7).
        at = max(i for i, s in enumerate(sections) if s[0] in (1, 2, 3, 4, 5, 6, 7, 8) or (s[0] == 0 and i == 0))
        sections.insert(at + 1, [9, enc(1) + seg])

    if funcs and not any(ent[0] == "table" for ent in entries):
        # No table import (wasm-ld leaves it out when nothing takes a function's address): one now.
        name_ = b"__indirect_function_table"
        entries.append(("table", b"\x03env" + enc(len(name_)) + name_ + b"\x01\x70", 0, enc(0)))
        if not imp:
            imp = [2, b""]
            at = max(i for i, s in enumerate(sections) if s[0] == 1 or (s[0] == 0 and i == 0))
            sections.insert(at + 1, imp)
    if imp:
        body = bytearray(enc(len(entries)))
        for ent in entries:
            if ent[0] == "raw":
                body += ent[1]
            else:
                _, head, flags, rest = ent
                # min, then max if there is one (rest holds the old min and max)
                _, q = leb(rest, 0)
                body += head + bytes([flags]) + enc(new_tsize) + rest[q:]
        imp[1] = bytes(body)

    if dynsym and exp:
        e = exp[1]
        count, p = leb(e, 0)
        kept = []
        for _ in range(count):
            start = p
            nme, p = name(e, p)
            kind = e[p]
            _, p = leb(e, p + 1)
            if kind == 4 or nme in KEEP:
                kept.append(e[start:p])
        sym_name = b"linux-wasm.dynsym"
        sections.append([0, enc(len(sym_name)) + sym_name + bytes(e)])
        exp[1] = enc(len(kept)) + b"".join(kept)

    nm, _ = name(dylink[1], 0)
    dylink[1] = enc(len(nm)) + nm + b"".join(bytes([s[0]]) + enc(len(s[1])) + s[1] for s in subs)
    out = bytearray(b"\0asm\1\0\0\0")
    for sid, body in sections:
        out += section(sid, bytes(body))
    open(path, "wb").write(out)
    print("%s: %d function exports in slots from %d (table %d -> %d)" % (path, len(funcs), tsize, tsize, new_tsize))


if __name__ == "__main__":
    main()
