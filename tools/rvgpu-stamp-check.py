#!/usr/bin/env python3
"""The deploy rule for rvgpu.wasm (VK-04's stamp, VK-05's gate), for the machines that have no Node: the build
container and the build VM run the release scripts with Python only. tools/rvgpu-stamp.js is the reference
(--require-clean); this reads the same wasm custom section "rvgpu-stamp" and applies the same rule.

  rvgpu-stamp-check.py FILE.wasm            print the stamp; exit 2 unless stamped, clean and with a full commit sha
  rvgpu-stamp-check.py --line FILE.wasm     one line for RELEASE.txt ("branch@commit mesa M dawn D patch P"), same exit rule
"""
import json
import re
import sys


def leb(b, i):
    v = s = 0
    while True:
        x = b[i]
        i += 1
        v += (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return v, i


def read(buf):
    if len(buf) < 8 or buf[:4] != b"\0asm":
        raise ValueError("not a wasm binary")
    i = 8
    while i < len(buf):
        sid = buf[i]
        i += 1
        size, i = leb(buf, i)
        end = i + size
        if end > len(buf):
            raise ValueError("truncated wasm section")
        if sid == 0:
            n, j = leb(buf, i)
            if buf[j:j + n] == b"rvgpu-stamp":
                return json.loads(buf[j + n:end].decode())
        i = end
    return None


def problems(st):
    if not st:
        return ["unstamped"]
    p = []
    if st.get("dirty") is not False:
        p.append("dirty build (uncommitted changes, or the commit was not known)")
    if not re.fullmatch(r"[0-9a-f]{40}", st.get("commit") or ""):
        p.append("no full commit sha")
    return p


def main(argv):
    line = "--line" in argv
    files = [a for a in argv if not a.startswith("--")]
    if len(files) != 1:
        print(__doc__, file=sys.stderr)
        return 64
    with open(files[0], "rb") as f:
        st = read(f.read())
    if line:
        if st:
            print("%s@%s mesa %s dawn %s patch %s" % (st.get("branch"), st.get("commit"), st.get("mesa"), st.get("dawn"),
                                                   st.get("patch")))
    elif st:
        print(files[0] + ":")
        for k in sorted(st):
            print("  %-7s %s" % (k, st[k]))
    else:
        print(files[0] + ": no rvgpu-stamp section (unstamped build)")
    pr = problems(st)
    if pr:
        print("rvgpu-stamp: refused: " + "; ".join(pr), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
