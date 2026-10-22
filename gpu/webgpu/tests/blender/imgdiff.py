# blender -b --factory-startup --python imgdiff.py -- A.png B.png [TOL] [SMALL_A.png SMALL_B.png]
# Pixel comparison of two renders with Blender's image API (no PIL in the chroot): pixels differing by more than
# TOL (of 255) in any channel, max difference, mean; optional downscaled copies to look at.
import bpy, sys
from array import array
argv = sys.argv[sys.argv.index("--") + 1:]
tol = int(argv[2]) if len(argv) > 2 else 8
ims = [bpy.data.images.load(p) for p in argv[:2]]
px = []
for im in ims:
    a = array('f', bytes(4 * im.size[0] * im.size[1] * 4))
    im.pixels.foreach_get(a)
    px.append(a)
if len(px[0]) != len(px[1]):
    print("imgdiff: sizes differ", ims[0].size[:], ims[1].size[:])
    sys.exit(0)
n = 0
mx = 0
tot = 0.0
A, B = px
for i in range(0, len(A), 4):
    d = max(abs(A[i] - B[i]), abs(A[i + 1] - B[i + 1]), abs(A[i + 2] - B[i + 2])) * 255.0
    if d > tol:
        n += 1
    if d > mx:
        mx = d
    tot += d
npx = len(A) // 4
print("imgdiff: %dx%d: %d of %d pixels differ by more than %d (%.2f%%), max %d, mean %.3f" %
      (ims[0].size[0], ims[0].size[1], n, npx, tol, 100.0 * n / npx, int(mx + 0.5), tot / npx))
if len(argv) > 4:
    for im, out in zip(ims, argv[3:5]):
        im.scale(im.size[0] // 3, im.size[1] // 3)
        im.filepath_raw = out
        im.file_format = 'PNG'
        im.save()
