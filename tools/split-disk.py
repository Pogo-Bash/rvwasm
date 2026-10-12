#!/usr/bin/env python3
"""Split a disk image into the pieces the page fetches on demand.

    split-disk.py IMAGE OUTDIR [PIECE_MIB]

Writes OUTDIR/manifest.json and one file per piece that is not all zeros,
named by its number in six digits (OUTDIR/000000, OUTDIR/000001, ...). Any
plain web server can serve that: no ranged requests are needed, and the
unused parts of a file system cost nothing.

The manifest:

    size     bytes in the image
    piece    bytes in a piece (the last one may be shorter)
    id       changes whenever the image does: the page keeps a copy of the
             pieces it has fetched, and what the system wrote, only as long
             as this matches
    present  one hexadecimal digit per four pieces, most significant bit
             first within a digit: 1 where there is a file, 0 where the piece
             is all zeros
    hashes   per piece, the sha256 of the piece in hexadecimal ("" for a piece
             that is all zeros). Pieces can be kept anywhere under these names
             (tools/r2-publish.py puts them on object storage and adds `base`,
             the address they are fetched from: BASE + hash + ".bin"; without
             `base` the page asks the site itself for the file named by number)
"""

import hashlib
import json
import os
import shutil
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    image, out = sys.argv[1], sys.argv[2]
    piece = (int(sys.argv[3]) if len(sys.argv) > 3 else 1) << 20
    size = os.path.getsize(image)

    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    digest = hashlib.sha256()
    zeros = bytes(piece)
    present = []
    hashes = []
    with open(image, "rb") as f:
        number = 0
        while True:
            data = f.read(piece)
            if not data:
                break
            digest.update(data)
            used = data != zeros[:len(data)]
            present.append(used)
            hashes.append(hashlib.sha256(data).hexdigest() if used else "")
            if used:
                with open(os.path.join(out, "%06d" % number), "wb") as o:
                    o.write(data)
            number += 1

    present += [False] * (-len(present) % 4)
    digits = "".join(
        "%x" % sum(bit << (3 - i) for i, bit in enumerate(present[n:n + 4]))
        for n in range(0, len(present), 4))
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump({"size": size, "piece": piece, "id": digest.hexdigest()[:32], "present": digits, "hashes": hashes}, f)
        f.write("\n")
    print("%s: %d pieces of %d MiB, %d with data" % (out, number, piece >> 20, sum(present)))


if __name__ == "__main__":
    main()
