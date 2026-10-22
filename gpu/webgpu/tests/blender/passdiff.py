# blender -b --factory-startup --python passdiff.py -- DIR_A DIR_B
# Every EXR of DIR_A against DIR_B's: pixels whose channels differ by more than 0.02 (relative to max(1, |b|)),
# max and mean absolute difference, and the mean of each (what a pass holds).
import bpy, sys, os
from array import array
a_dir, b_dir = sys.argv[sys.argv.index("--") + 1:][:2]
for f in sorted(os.listdir(a_dir)):
    if not f.endswith(".exr") or not os.path.exists(os.path.join(b_dir, f)):
        continue
    px = []
    for d in (a_dir, b_dir):
        im = bpy.data.images.load(os.path.join(d, f))
        im.scale(480, 270)	# (pure Python: a quarter of each axis is enough to see which pass is off)
        a = array('f', bytes(4 * im.size[0] * im.size[1] * 4))
        im.pixels.foreach_get(a)
        px.append(a)
    A, B = px
    n = 0; mx = 0.0; tot = 0.0; ma = 0.0; mb = 0.0; bad = 0
    for i in range(0, len(A), 4):
        d = 0.0
        for c in range(3):
            x, y = A[i + c], B[i + c]
            if x != x or y != y or abs(x) == float("inf") or abs(y) == float("inf"):
                bad += 1
                continue
            d = max(d, abs(x - y) / max(1.0, abs(y)))
            ma += x; mb += y
        if d > 0.02:
            n += 1
        mx = max(mx, d); tot += d
    npx = len(A) // 4
    print("passdiff %-28s %6.2f%% differ, max %.3f, mean %.4f; means %.4f vs %.4f; non-finite %d" %
          (f, 100.0 * n / npx, mx, tot / npx, ma / (3 * npx), mb / (3 * npx), bad))
