# blender -b --factory-startup --python pix.py -- FILE...   pixel values at a few spots (x, y from bottom-left)
import bpy, sys
for f in sys.argv[sys.argv.index("--") + 1:]:
    im = bpy.data.images.load(f)
    w, h = im.size
    out = []
    for (x, y) in ((50, 50), (w // 2, h // 2), (w // 2, int(h * 0.8)), (int(w * 0.45), int(h * 0.4))):
        i = (y * w + x) * 4
        out.append("(%d,%d)=%s" % (x, y, ",".join("%.4g" % v for v in im.pixels[i:i + 4])))
    print("pix", f.split("/")[-2] + "/" + f.split("/")[-1], " ".join(out))
