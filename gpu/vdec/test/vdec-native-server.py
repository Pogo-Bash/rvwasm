#!/usr/bin/env python3
# A native stand-in for gpuserver and the page together, for testing rvwasm_drv_video.so in the build VM
# (gpu/vdec/test/native.sh): the vtest commands rv's render node needs (rv/virtgpu.c: connect, resources,
# PRIME import across opens) and the video commands (gpu/server/vdec.js), with a stateful decoder standing in
# for WebCodecs (vdec-ref, libavcodec, under qemu-riscv64). Frames are copied into images as the page would;
# "drawn" frames (zero copy) are only marked as drawn.
#   vdec-native-server.py SOCKET [--gpu] [--ref "qemu-riscv64-static -L ROOT .../vdec-ref"] [--log FILE]
import argparse, mmap, os, select, socket, struct, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("socket")
ap.add_argument("--gpu", action="store_true", help="say frames can be drawn into GPU resources (zero copy)")
ap.add_argument("--ref", required=True, help="the command that runs vdec-ref")
ap.add_argument("--caps", default="1,2,3,4,5", help="codecs offered (1 H.264, 2 HEVC, 3 VP8, 4 VP9, 5 AV1)")
ap.add_argument("--log", default=None)
ap.add_argument("--max", default="4096x2304")
ap.add_argument("--reorder", type=int, default=0, help="H.264's reordering (vdec-ref.c says why)")
ap.add_argument("--no-decoder", action="store_true",
                help="answer VDEC_HELLO as gpuserver does while the page's decoder has not said anything (1): the driver's init fails")
args = ap.parse_args()
logf = open(args.log, "a") if args.log else sys.stderr
def log(*a):
    print("native-server:", *a, file=logf, flush=True)

ST_BEAT, ST_STATE, ST_CONSUMED, ST_SLOTS, ST_SLOT_SIZE, ST_MAX_SLOTS = 4, 8, 12, 4096, 32, 4096
ST_SIZE = ST_SLOTS + ST_SLOT_SIZE * ST_MAX_SLOTS
V = dict(GET_CAPS2=9, CREATE_RENDERER=8, PING=10, PROTOCOL_VERSION=11, BUSY_WAIT=7, RESOURCE_CREATE2=12,
         RESOURCE_UNREF=3, SUBMIT_CMD=6, TRANSFER_GET2=13, TRANSFER_PUT2=14, RV_FENCE=0x10001, RV_IMPORT=0x10002,
         HELLO=0x10010, ALLOC=0x10011, FREE=0x10012, STREAM=0x10013, STREAM_END=0x10014, SURFACE=0x10015,
         DECODE=0x10016, COPY=0x10017, EXPORT=0x10018, SLOT_END=0x10019)
SIZES = {V["PING"]: 0, V["PROTOCOL_VERSION"]: 1, V["BUSY_WAIT"]: 2, V["RESOURCE_CREATE2"]: 11, V["RESOURCE_UNREF"]: 1,
         V["TRANSFER_GET2"]: 10, V["TRANSFER_PUT2"]: 10, V["GET_CAPS2"]: 0, V["RV_FENCE"]: 0, V["RV_IMPORT"]: 3,
         V["HELLO"]: 1, V["ALLOC"]: 1, V["FREE"]: 1, V["STREAM"]: 7, V["STREAM_END"]: 1, V["SURFACE"]: 5,
         V["DECODE"]: 8, V["COPY"]: 11, V["EXPORT"]: 1, V["SLOT_END"]: 1}

def memfile(size):
    fd = os.memfd_create("vdec", 0)
    os.ftruncate(fd, size)
    return fd, mmap.mmap(fd, size)

resources = {}		# res -> dict(fd, ino, dev, target, format, bind, w, h, holders)
next_res = [1]
mems = {}		# mem id -> (fd, mmap, size)
next_mem = [1]
stats = dict(chunks=0, frames=0, copies=0, imports=0, errors=0)

class Stream:
    def __init__(self, client, sid, codec):
        self.client, self.sid, self.codec = client, sid, codec
        self.proc = subprocess.Popen(args.ref, shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.pending = {}
        self.need_key = True
        self.last = time.time()
        r = self.op(1, codec, args.reorder if codec == 1 else 0, b"")
        log("stream", sid, "codec", codec, "decoder", "ready" if r == 1 else "FAILED")
    def op(self, op, a, b, data):
        self.proc.stdin.write(struct.pack("<4I", op, a, b, len(data)) + data)
        self.proc.stdin.flush()
        while True:
            h = self.proc.stdout.read(16)
            if len(h) < 16:
                return 2
            ts, w, hh, status = struct.unpack("<4I", h)
            if w == 0:
                return status
            nv12 = self.proc.stdout.read(w * hh + (w & ~1) * (hh // 2))
            self.client.frame(self, ts, w, hh, nv12)
    def end(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(5)
        except Exception:
            self.proc.kill()

class Client:
    def __init__(self, sock):
        self.sock, self.buf, self.video = sock, b"", False
        self.status = None
        self.streams, self.slots, self.frames, self.res = {}, {}, {}, set()
    def word(self, off, v):
        struct.pack_into("<I", self.status, off, v & 0xffffffff)
    def slot_word(self, slot, i, v):
        self.word(ST_SLOTS + ST_SLOT_SIZE * slot + 4 * i, v)
    def reply(self, cmd, words, fd=None):
        self.sock.sendall(struct.pack("<2I", len(words), cmd) + struct.pack("<%dI" % len(words), *[w & 0xffffffff for w in words]))
        if fd is not None:
            socket.send_fds(self.sock, [b"\0"], [fd])
    def frame(self, stream, ts, w, h, nv12):
        p = stream.pending.pop(ts, None)
        if p is None:
            log("frame with timestamp", ts, "that no chunk had")
            return
        slot, flags = p
        self.frames[slot] = (ts, w, h, nv12)
        stats["frames"] += 1
        self.slot_word(slot, 0, ts)
        if flags & 4:
            self.slot_word(slot, 1, ts)	# "drawn"
    def command(self, cmd, a):
        if cmd == V["CREATE_RENDERER"]:
            return
        if cmd == V["PING"]:
            return self.reply(cmd, [])
        if cmd == V["BUSY_WAIT"]:
            return self.reply(cmd, [0])
        if cmd == V["PROTOCOL_VERSION"]:
            return self.reply(cmd, [3])
        if cmd == V["RESOURCE_CREATE2"]:
            size = a[10]
            res = next_res[0]; next_res[0] += 1
            fd, m = memfile(max(size, 1))
            st = os.fstat(fd)
            resources[res] = dict(fd=fd, m=m, dev=st.st_dev, ino=st.st_ino, target=a[1], format=a[2], bind=a[3],
                                  w=a[4], h=a[5], size=size, holders={self})
            self.res.add(res)
            self.reply(cmd, [res])
            if size:
                socket.send_fds(self.sock, [b"\0"], [fd])
            return
        if cmd == V["RESOURCE_UNREF"]:
            r = resources.get(a[0])
            if r and self in r["holders"]:
                r["holders"].discard(self)
                self.res.discard(a[0])
                if not r["holders"]:
                    os.close(r["fd"]); del resources[a[0]]
            return
        if cmd == V["RV_IMPORT"]:
            ino = a[1] | (a[2] << 32)
            for res, r in resources.items():
                if (r["dev"] & 0xffffffff) == a[0] and r["ino"] == ino:
                    r["holders"].add(self); self.res.add(res)
                    stats["imports"] += 1
                    return self.reply(cmd, [res, r["target"], r["format"], r["bind"], r["w"], r["h"], 1, 1, 0, r["size"]])
            return self.reply(cmd, [0] * 10)
        if cmd in (V["SUBMIT_CMD"], V["TRANSFER_GET2"], V["TRANSFER_PUT2"]):
            return
        if cmd == V["GET_CAPS2"]:
            caps = bytes(4096)
            self.sock.sendall(struct.pack("<2I", len(caps) + 1, 2) + caps)
            return
        # ---- video
        if cmd == V["HELLO"] and args.no_decoder:
            return self.reply(cmd, [1, 0])
        if cmd == V["HELLO"]:
            self.video = True
            fd, self.status = memfile(ST_SIZE)
            self.status_fd = fd
            self.word(0, 0x53565652); self.word(ST_STATE, 1)
            mw, mh = (int(x) for x in args.max.split("x"))
            caps = []
            for i, c in enumerate(int(x) for x in args.caps.split(",")):
                caps += [c, 0, mw, mh, 1 | (4 if args.gpu and i == 0 else 0)]
            self.reply(cmd, [0, len(caps) // 5] + caps, fd)
            return
        if cmd == V["ALLOC"]:
            mid = next_mem[0]; next_mem[0] += 1
            fd, m = memfile(a[0])
            mems[mid] = (fd, m, a[0])
            return self.reply(cmd, [mid, 0], fd)
        if cmd == V["FREE"]:
            e = mems.pop(a[0], None)
            if e:
                os.close(e[0])
            return
        if cmd == V["STREAM"]:
            self.streams[a[0]] = Stream(self, a[0], a[1])
            log("stream", a[0], "profile", a[2], "level", a[3], "size %dx%d" % (a[4], a[5]))
            return
        if cmd == V["STREAM_END"]:
            s = self.streams.pop(a[0], None)
            if s:
                s.end()
            return
        if cmd == V["SURFACE"]:
            self.slots[a[0]] = (a[3], a[4])
            return
        if cmd == V["SLOT_END"]:
            self.slots.pop(a[0], None); self.frames.pop(a[0], None)
            return
        if cmd == V["DECODE"]:
            sid, slot, seq, mid, off, size, flags, end = a
            data = bytes(mems[mid][1][off:off + size])
            self.word(ST_CONSUMED, end)
            stats["chunks"] += 1
            s = self.streams[sid]
            if s.need_key and not (flags & 1):
                log("chunk", seq, "is not a key chunk after configure: dropped")
                self.slot_word(slot, 4, seq); stats["errors"] += 1
                return
            s.need_key = False
            if not (flags & 2):
                s.pending[seq] = (slot, flags)
            s.last = time.time()
            r = s.op(2, seq, flags & 1, data)
            if r != 1:
                stats["errors"] += 1
                log("chunk", seq, "failed to decode")
                for t, (sl, fl) in list(s.pending.items()):
                    self.slot_word(sl, 4, t)
                s.pending.clear()
            return
        if cmd == V["EXPORT"]:
            f = self.frames.get(a[0])
            if f:
                self.slot_word(a[0], 1, f[0])
            return
        if cmd == V["COPY"]:
            slot, cid, seq, mid, oy, py, ouv, puv, w, h, size = a
            f = self.frames.get(slot)
            m = mems[mid][1]
            if not f or f[0] != seq:
                log("copy of slot", slot, "chunk", seq, "but the frame there is", f and f[0])
                self.slot_word(slot, 3, 1); self.slot_word(slot, 2, cid)
                return
            _, fw, fh, nv12 = f
            cw, ch = min(w, fw), min(h, fh)
            for y in range(ch):
                m[oy + y * py: oy + y * py + cw] = nv12[y * fw: y * fw + cw]
            base = fw * fh
            for y in range(ch // 2):
                m[ouv + y * puv: ouv + y * puv + (cw & ~1)] = nv12[base + y * (fw & ~1): base + y * (fw & ~1) + (cw & ~1)]
            stats["copies"] += 1
            self.slot_word(slot, 3, 0); self.slot_word(slot, 2, cid)
            return
        log("unknown command", hex(cmd))
    def feed(self, data):
        self.buf += data
        while len(self.buf) >= 8:
            n, cmd = struct.unpack_from("<2I", self.buf)
            size = n if cmd == V["CREATE_RENDERER"] else 4 * SIZES.get(cmd, n)
            if len(self.buf) < 8 + size:
                return
            a = struct.unpack_from("<%dI" % (size // 4), self.buf, 8) if cmd != V["CREATE_RENDERER"] else ()
            self.buf = self.buf[8 + size:]
            self.command(cmd, list(a))
    def close(self):
        for s in self.streams.values():
            s.end()
        for res in list(self.res):
            r = resources.get(res)
            if r:
                r["holders"].discard(self)
                if not r["holders"]:
                    del resources[res]

try:
    os.unlink(args.socket)
except FileNotFoundError:
    pass
ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
ls.bind(args.socket)
os.chmod(args.socket, 0o777)
ls.listen(16)
log("listening on", args.socket)
clients = {}
beat = 0
last = time.time()
while True:
    r, _, _ = select.select([ls] + [c.sock for c in clients.values()], [], [], 0.05)
    for s in r:
        if s is ls:
            c, _ = ls.accept()
            clients[c.fileno()] = Client(c)
            continue
        cl = clients[s.fileno()]
        try:
            d = s.recv(1 << 20)
        except ConnectionResetError:
            d = b""
        if not d:
            cl.close(); del clients[s.fileno()]; s.close()
            continue
        cl.feed(d)
    beat += 1
    # (the stand-in holds H.264 pictures for its reordering; when nothing more comes, as at the end of a file,
    # they are flushed out: Chrome's decoder would have given them already)
    for cl in clients.values():
        for st in cl.streams.values():
            if st.pending and time.time() - st.last > 0.3:
                st.op(3, 0, 0, b"")
                st.last = time.time() + 1e9
    for cl in clients.values():
        if cl.status is not None:
            cl.word(ST_BEAT, beat)
    if time.time() - last > 5:
        last = time.time()
        log("stats", stats, "clients", len(clients), "resources", len(resources))
