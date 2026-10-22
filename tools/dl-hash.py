#!/usr/bin/env python3
"""dl-hash.py FILE...: the content key of a library as the runtime computes it (patch-runtime.py item 26, dl_key), for
the manifest of libraries a page compiles ahead (os.dl.preload). Two 32-bit lanes over the words of the file (the last
partial word zero-padded), position dependent, and the length: "<8 hex><8 hex>-<length>". Prints "KEY  FILE"."""
import struct
import sys

M = 0xffffffff


def key(data):
    n = len(data)
    words = n // 4
    h1, h2 = 0x9e3779b9, 0x85ebca6b
    vals = struct.unpack("<%dI" % words, data[:4 * words])
    if n % 4:
        vals = vals + (int.from_bytes(data[4 * words:], "little"),)
    for i, w in enumerate(vals):
        h1 = ((h1 ^ w) * 0x9e3779b1) & M
        h1 ^= h1 >> 15
        h2 = (((h2 + w + i) & M) * 0x85ebca6b) & M
        h2 ^= h2 >> 13
    h1 = (((h1 ^ (h1 >> 16) ^ n) & M) * 0x7feb352d) & M
    h2 = (((h2 ^ (h2 >> 16) ^ h1) & M) * 0x846ca68b) & M
    return "%08x%08x-%d" % (h1, h2, n)


if __name__ == "__main__":
    for path in sys.argv[1:]:
        print(key(open(path, "rb").read()), path)
