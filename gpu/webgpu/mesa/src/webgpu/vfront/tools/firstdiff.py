#!/usr/bin/env python3
"""firstdiff.py DIR_A DIR_B [THRESHOLD_PERCENT]: the first image (by name) whose share of pixels differing by
more than 8/255 is above the threshold (default 0.3%), for VFRONT_DRAWDUMP directories of two runs."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgcmp import read_ppm, compare  # noqa: E402

a, b = sys.argv[1], sys.argv[2]
thr = float(sys.argv[3]) / 100 if len(sys.argv) > 3 else 0.003
names = sorted(n for n in os.listdir(a) if os.path.exists(os.path.join(b, n)))
print('%d images in common' % len(names))
for n in names:
    wa, ha, x = read_ppm(os.path.join(a, n))
    wb, hb, y = read_ppm(os.path.join(b, n))
    if (wa, ha) != (wb, hb):
        print(n, 'sizes differ', wa, ha, wb, hb)
        continue
    m, mean, frac, psnr = compare(x, y, 8)
    if frac > thr:
        print('%s %dx%d differing %.2f%% max %d' % (n, wa, ha, frac * 100, m))
        sys.exit(0)
print('none above %.2f%%' % (thr * 100))
