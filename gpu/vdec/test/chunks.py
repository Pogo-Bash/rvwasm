#!/usr/bin/env python3
# What the driver sent (RVVDEC_DUMP's file: {u32 stream, u32 codec, u32 flags, u32 size} + bytes per chunk):
#   chunks.py FILE                 counts
#   chunks.py FILE --es OUT        the chunks of the first stream as an elementary stream (H.264/HEVC: Annex B
#                                  as it is; VP8/VP9/AV1: IVF), for decoding with ffmpeg on its own
import struct, sys
f = open(sys.argv[1], "rb").read()
at, chunks = 0, []
while at + 16 <= len(f):
    stream, codec, flags, size = struct.unpack_from("<4I", f, at)
    chunks.append((stream, codec, flags, f[at + 16:at + 16 + size]))
    at += 16 + size
if "--es" in sys.argv:
    out = open(sys.argv[sys.argv.index("--es") + 1], "wb")
    first = [c for c in chunks if c[0] == chunks[0][0]]
    codec = first[0][1]
    if codec in (1, 2):
        for c in first:
            out.write(c[3])
    else:
        fourcc = {3: b"VP80", 4: b"VP90", 5: b"AV01"}[codec]
        out.write(b"DKIF" + struct.pack("<HH4sHHIII", 0, 32, fourcc, 0, 0, 30, 1, len(first), 0))
        for i, c in enumerate(first):
            out.write(struct.pack("<IQ", len(c[3]), i) + c[3])
names = {1: "H.264", 2: "HEVC", 3: "VP8", 4: "VP9", 5: "AV1"}
key = sum(1 for c in chunks if c[2] & 1)
hidden = sum(1 for c in chunks if c[2] & 2)
draw = sum(1 for c in chunks if c[2] & 4)
codecs = sorted(set(names.get(c[1], "?") for c in chunks))
print("chunks: %d (%s; %d key, %d hidden, %d drawn), %d bytes" % (len(chunks), "/".join(codecs) or "-", key, hidden, draw,
      sum(len(c[3]) for c in chunks)))
