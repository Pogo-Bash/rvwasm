#!/bin/bash
# What reached the page (vdec-stub.js's VDEC_STUB_DUMP file, or the driver's RVVDEC_DUMP), decoded on its own
# by the stateful stand-in (vdec-ref: libavcodec, chunk by chunk, with the original's reordering for H.264, as
# vdec-ref.c says why), against the original file decoded by ffmpeg: frame by frame (MD5 of NV12).
#   gpu/vdec/test/check-dump.sh CHUNKS ORIGINAL [FRAMES]       (in the VM)
set -e
T=$(cd "$(dirname "$0")" && pwd)
W=${W:-$HOME/vdec}; R=$HOME/rvwasm/debian-root-kde; X=$W/x
LDP=$X/usr/lib/riscv64-linux-gnu:$R/usr/lib/riscv64-linux-gnu/blas:$R/usr/lib/riscv64-linux-gnu/lapack
Q="qemu-riscv64-static -L $R -E LD_LIBRARY_PATH=$LDP"
O=/tmp/vdec-check; mkdir -p $O
reorder=$($Q $X/usr/bin/ffprobe -v error -select_streams v:0 -show_entries stream=has_b_frames -of csv=p=0 "$2" < /dev/null)
size=$($Q $X/usr/bin/ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "$2" < /dev/null)
$Q $X/usr/bin/ffmpeg -y -nostdin -v error -i "$2" ${3:+-frames:v $3} -an -pix_fmt nv12 -f framemd5 $O/orig.md5 < /dev/null
python3 - "$1" "$Q $W/out/vdec-ref" "${reorder:-0}" $O/orig.md5 "${3:-0}" "$size" <<'PY'
import hashlib, struct, subprocess, sys
chunks, ref, reorder, orig, limit = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
# (the chunks' pictures are whole macroblocks or CTBs, as VA's surfaces: the player crops; so does this)
ow, oh = (int(x) for x in sys.argv[6].split("x"))
data = open(chunks, "rb").read()
at, cs = 0, []
while at + 16 <= len(data):
    stream, codec, flags, size = struct.unpack_from("<4I", data, at)
    cs.append((codec, flags, data[at + 16:at + 16 + size])); at += 16 + size
p = subprocess.Popen(ref, shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
got = []
def op(o, a, b, payload):
    p.stdin.write(struct.pack("<4I", o, a, b, len(payload)) + payload); p.stdin.flush()
    while True:
        ts, w, h, st = struct.unpack("<4I", p.stdout.read(16))
        if w == 0:
            return st
        f = p.stdout.read(w * h + (w & ~1) * (h // 2))
        y = b"".join(f[r * w: r * w + ow] for r in range(oh))
        uv = b"".join(f[w * h + r * (w & ~1): w * h + r * (w & ~1) + ow] for r in range(oh // 2))
        got.append(hashlib.md5(y + uv).hexdigest())
op(1, cs[0][0], reorder if cs[0][0] == 1 else 0, b"")
for i, (codec, flags, payload) in enumerate(cs):
    op(2, i + 1, flags & 1, payload)
op(3, 0, 0, b"")
want = [l.strip().split(",")[-1].strip() for l in open(orig) if not l.startswith("#")]
if limit:
    got = got[:limit]
same = sum(1 for a, b in zip(got, want) if a == b)
print("frames: original %d, from what the page got %d, the same %d" % (len(want), len(got), same))
print("PASS (the page got the original's pictures exactly)" if want and same == len(want) else "FAIL")
PY
