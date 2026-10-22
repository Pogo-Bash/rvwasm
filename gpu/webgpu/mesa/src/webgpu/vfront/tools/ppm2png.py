#!/usr/bin/env python3
"""ppm2png.py OUT.png A.ppm [B.ppm]: one image as PNG, or A | B | difference (x4) side by side."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgcmp import read_ppm, write_png, side_by_side  # noqa: E402

out = sys.argv[1]
w, h, a = read_ppm(sys.argv[2])
if len(sys.argv) > 3:
    _, _, b = read_ppm(sys.argv[3])
    write_png(out, *side_by_side(w, h, a, b))
else:
    write_png(out, w, h, a)
