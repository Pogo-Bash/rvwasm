#!/usr/bin/env python3
"""The pieces a cold boot reads, in the order it reads them: the list the disk worker fetches ahead (`boot` in the
disk's manifest.json, see tools/www/disk-worker.js "Fetching ahead").

    tools/boot-list.py TRACE [--disk release] [--manifest FILE-OR-URL] [-o docker/boot/release.json] [--until N]

TRACE is the output of a cold boot with the disk worker's trace on, which is what tools/boot-trace.sh makes: lines
    trace <ms since the worker started> <disk> <piece> <ms the piece took, -1 if it was here> <how>
(the first time each piece is touched; anything around them is ignored). The result is written as JSON:
    { "disk": "release", "id": <the disk's id>, "boot": [pieces in the order of first touch], "hashes": [their sha256] }
tools/r2-publish.py puts `boot` in the manifest of the dist it publishes. It is by the disk's `id`; when a later
release has another id (the root changed), the list is carried over by `hashes` instead: the pieces both disks have
stay in it, in the same order, at their new numbers, and the ones that changed are dropped. A list from a boot of
the old disk is thus a good start until the next trace.

--manifest is the manifest of the disk that was booted (a file, or the site's URL): its id and the pieces' hashes.
--until N keeps the first N pieces only.
"""
import argparse
import json
import re
import sys
import urllib.request


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("trace")
    p.add_argument("--disk", default="release")
    p.add_argument("--manifest", required=True)
    p.add_argument("-o", "--output")
    p.add_argument("--until", type=int)
    a = p.parse_args()

    if a.manifest.startswith("http"):
        with urllib.request.urlopen(a.manifest, timeout=60) as r:
            m = json.load(r)
    else:
        with open(a.manifest) as f:
            m = json.load(f)
    hashes = m.get("hashes")
    if not hashes:
        print("boot-list: the manifest has no hashes", file=sys.stderr)
        return 1

    boot = []
    seen = set()
    line_re = re.compile(r"\btrace (\d+) (\S+) (\d+) (-?\d+) (\w+)")
    last = 0
    with open(a.trace, errors="replace") as f:
        for line in f:
            found = line_re.search(line)
            if not found or found.group(2) != a.disk:
                continue
            piece = int(found.group(3))
            if piece in seen or piece >= len(hashes) or not hashes[piece]:
                continue
            seen.add(piece)
            boot.append(piece)
            last = int(found.group(1))
    if a.until:
        boot = boot[:a.until]
    if not boot:
        print("boot-list: no trace lines for disk %s in %s" % (a.disk, a.trace), file=sys.stderr)
        return 1
    out = {"disk": a.disk, "id": m["id"], "boot": boot, "hashes": [hashes[i] for i in boot]}
    text = json.dumps(out, separators=(",", ":")) + "\n"
    if a.output:
        with open(a.output, "w") as f:
            f.write(text)
    print("boot-list: %d pieces (%.0f MiB) of disk %s id %s, the last touched %.0f s into the boot%s" % (
        len(boot), len(boot) * m["piece"] / 1048576, a.disk, m["id"], last / 1000, "; " + a.output if a.output else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
