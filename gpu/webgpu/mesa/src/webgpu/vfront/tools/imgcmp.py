#!/usr/bin/env python3
"""Compare the images two replays of the same capture wrote (vfront-replay and vrend-replay, tools/).

    imgcmp.py DIR_A DIR_B [--tolerance N] [--png OUT_DIR]

For each get-NNNNN.ppm present in both: the largest channel difference, the mean difference, the share of
pixels differing by more than the tolerance (default 8 of 255, for rasterisation and precision differences
between two GL implementations), and PSNR. An image "matches" when under 1% of its pixels differ by more
than the tolerance. --png writes A | B | difference side by side for the worst image and the last one.
No dependencies beyond the standard library.
"""
import math
import os
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = []
    i = 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def compare(a, b, tol):
    n = len(a) // 3
    maxd = 0
    total = 0
    bad = 0
    sq = 0
    for p in range(n):
        d = 0
        for c in range(3):
            x = abs(a[p * 3 + c] - b[p * 3 + c])
            total += x
            sq += x * x
            if x > d:
                d = x
        if d > maxd:
            maxd = d
        if d > tol:
            bad += 1
    mse = sq / max(1, n * 3)
    psnr = float('inf') if mse == 0 else 10 * math.log10(255 * 255 / mse)
    return maxd, total / max(1, n * 3), bad / max(1, n), psnr


def side_by_side(w, h, a, b):
    out = bytearray(w * 3 * h * 3)
    for y in range(h):
        row = y * w * 3 * 3
        out[row:row + w * 3] = a[y * w * 3:(y + 1) * w * 3]
        out[row + w * 3:row + w * 6] = b[y * w * 3:(y + 1) * w * 3]
        for x in range(w * 3):
            out[row + w * 6 + x] = min(255, abs(a[y * w * 3 + x] - b[y * w * 3 + x]) * 4)
    return w * 3, h, bytes(out)


def main():
    args = sys.argv[1:]
    tol = 8
    png = None
    rest = []
    i = 0
    while i < len(args):
        if args[i] == '--tolerance':
            tol = int(args[i + 1])
            i += 2
        elif args[i] == '--png':
            png = args[i + 1]
            i += 2
        else:
            rest.append(args[i])
            i += 1
    if len(rest) != 2:
        print(__doc__)
        return 2
    da, db = rest
    names = sorted(n for n in os.listdir(da) if n.endswith('.ppm') and os.path.exists(os.path.join(db, n)))
    only_a = sorted(set(n for n in os.listdir(da) if n.endswith('.ppm')) - set(names))
    if not names:
        print('no images in common (%s: %d, %s: %d)' % (da, len(os.listdir(da)), db, len(os.listdir(db))))
        return 1
    matched = 0
    worst = None
    for n in names:
        wa, ha, a = read_ppm(os.path.join(da, n))
        wb, hb, b = read_ppm(os.path.join(db, n))
        if (wa, ha) != (wb, hb) or len(a) != len(b):
            print('%s: sizes differ' % n)
            continue
        maxd, mean, frac, psnr = compare(a, b, tol)
        ok = frac < 0.01
        matched += ok
        if worst is None or frac > worst[1]:
            worst = (n, frac, wa, ha, a, b)
        print('%s %dx%d: max %3d mean %6.3f differing %6.2f%% psnr %6.1f %s' %
              (n, wa, ha, maxd, mean, frac * 100, psnr, 'ok' if ok else 'DIFFERENT'))
    print('%d of %d images match (tolerance %d)%s' % (matched, len(names), tol,
          ', %d only in %s' % (len(only_a), da) if only_a else ''))
    if png and worst:
        os.makedirs(png, exist_ok=True)
        n, frac, w, h, a, b = worst
        write_png(os.path.join(png, 'worst-' + n.replace('.ppm', '.png')), *side_by_side(w, h, a, b))
        wl, hl, al = read_ppm(os.path.join(da, names[-1]))
        _, _, bl = read_ppm(os.path.join(db, names[-1]))
        write_png(os.path.join(png, 'last-' + names[-1].replace('.ppm', '.png')), *side_by_side(wl, hl, al, bl))
    return 0 if matched == len(names) else 1


if __name__ == '__main__':
    sys.exit(main())
