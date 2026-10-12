#!/usr/bin/env python3
# A Wayland client that shows one window whose buffer comes through zwp_linux_dmabuf_v1, for trying
# wlserver/dmabuf.js without a Vulkan driver. Two kinds of buffer:
#   dmabuf-test.py memfd [SECONDS]   a file in memory with a picture in it (drawn like a wl_shm buffer)
#   dmabuf-test.py gpu [SECONDS]     a GPU resource: the backing file of a resource made at the GPU server
#                                    (gpu/server/, VTEST_SOCKET_NAME) with vtest, handed to the window server,
#                                    which finds it in the GPU server's export table (slot format 2)
# Speaks the protocols by hand (no libwayland, no Mesa): the wire format is ids, opcodes and padded words.
import os, socket, struct, sys, time, array

mode = sys.argv[1] if len(sys.argv) > 1 else "memfd"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10
W, H = 256, 192
STRIDE = W * 4
XR24 = 0x34325258

def log(*a):
    print("dmabuf-test:", *a, flush=True)

# ---- a buffer
def gpu_resource():
    path = os.environ.get("VTEST_SOCKET_NAME", os.environ.get("XDG_RUNTIME_DIR", "/tmp") + "/rvgpu-0")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    def send(cmd, words=(), raw=None):
        payload = raw if raw is not None else struct.pack("<%dI" % len(words), *words)
        length = len(payload) if cmd == 8 else len(payload) // 4
        s.sendall(struct.pack("<II", length, cmd) + payload)
    def recv(n):
        b = b""
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise SystemExit("dmabuf-test: the GPU server closed the connection")
            b += c
        return b
    send(8, raw=b"dmabuf-test\0")                 # CREATE_RENDERER
    send(10); send(7, (0, 0))                       # PING_PROTOCOL_VERSION, BUSY_WAIT 0
    assert struct.unpack("<II", recv(8))[1] == 10
    l, c = struct.unpack("<II", recv(8)); recv(4 * l)
    send(11, (3,))                                  # PROTOCOL_VERSION 3
    recv(8); version, = struct.unpack("<I", recv(4))
    send(9)                                         # GET_CAPS2
    l, c = struct.unpack("<II", recv(8)); recv(l - 1)
    # RESOURCE_CREATE2: handle 0, PIPE_TEXTURE_2D (2), VIRGL_FORMAT_B8G8R8X8_UNORM (2), the binds below
    # (virgl_hw.h), 1 deep, 1 layer, no mip levels, no samples, the backing's size.
    VIRGL_BIND_RENDER_TARGET, VIRGL_BIND_SAMPLER_VIEW, VIRGL_BIND_DISPLAY_TARGET = 1 << 1, 1 << 3, 1 << 7
    bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_DISPLAY_TARGET
    send(12, (0, 2, 2, bind, W, H, 1, 1, 0, 0, STRIDE * H))
    l, c = struct.unpack("<II", recv(8)); res, = struct.unpack("<I", recv(4))
    fds = array.array("i")
    msg, anc, flags, addr = s.recvmsg(1, socket.CMSG_LEN(4))
    for level, kind, data in anc:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            fds.frombytes(data[:4])
    log("vtest protocol", version, "resource", res, "backing fd", fds[0])
    return fds[0], s, res

def picture(fd, frame):
    rows = []
    for y in range(H):
        row = bytearray(STRIDE)
        for x in range(0, W, 16):
            b, g, r = (x + frame * 8) & 255, (y + frame * 4) & 255, 128
            row[x * 4:(x + 16) * 4] = bytes((b, g, r, 255)) * 16
        rows.append(bytes(row))
    os.pwrite(fd, b"".join(rows), 0)

keep = None
if mode == "gpu":
    fd, keep, res = gpu_resource()
else:
    fd = os.memfd_create("dmabuf-test")
    os.ftruncate(fd, STRIDE * H)
    picture(fd, 0)

# ---- Wayland
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(os.environ.get("XDG_RUNTIME_DIR", "/tmp/runtime-root") + "/" + os.environ.get("WAYLAND_DISPLAY", "wayland-0"))
next_id = [2]
def new():
    i = next_id[0]; next_id[0] += 1; return i
def string(t):
    raw = t.encode() + b"\0"
    return struct.pack("<I", len(raw)) + raw + b"\0" * ((4 - len(raw) % 4) % 4)
def req(obj, op, payload=b"", fds=()):
    data = struct.pack("<II", obj, ((8 + len(payload)) << 16) | op) + payload
    if fds:
        sock.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))])
    else:
        sock.sendall(data)
inbuf = b""
def events(timeout):
    global inbuf
    sock.settimeout(timeout)
    try:
        inbuf += sock.recv(65536)
    except socket.timeout:
        pass
    out = []
    while len(inbuf) >= 8:
        obj, so = struct.unpack_from("<II", inbuf)
        size, op = so >> 16, so & 0xffff
        if len(inbuf) < size:
            break
        out.append((obj, op, inbuf[8:size]))
        inbuf = inbuf[size:]
    return out

registry = new()
req(1, 1, struct.pack("<I", registry))      # wl_display.get_registry
cb = new()
req(1, 0, struct.pack("<I", cb))            # wl_display.sync
globals_ = {}
done = False
while not done:
    for obj, op, body in events(5):
        if obj == registry and op == 0:
            name, = struct.unpack_from("<I", body)
            n, = struct.unpack_from("<I", body, 4)
            iface = body[8:8 + n - 1].decode()
            ver, = struct.unpack_from("<I", body, 8 + ((n + 3) & ~3))
            globals_[iface] = (name, ver)
        elif obj == cb:
            done = True
        elif obj == 1 and op == 0:
            raise SystemExit("dmabuf-test: protocol error " + repr(body))
if "zwp_linux_dmabuf_v1" not in globals_:
    raise SystemExit("dmabuf-test: the window server offers no zwp_linux_dmabuf_v1 (WLSERVER_DMABUF=1?)")
def bind(iface, version):
    i = new()
    req(registry, 0, struct.pack("<I", globals_[iface][0]) + string(iface) + struct.pack("<II", version, i))
    return i
compositor = bind("wl_compositor", 4)
wm = bind("xdg_wm_base", 1)
dmabuf = bind("zwp_linux_dmabuf_v1", 3)
surface = new(); req(compositor, 0, struct.pack("<I", surface))
xsurf = new(); req(wm, 2, struct.pack("<II", xsurf, surface))
top = new(); req(xsurf, 1, struct.pack("<I", top))
req(top, 2, string("dmabuf-test (" + mode + ")"))
req(surface, 6)                               # commit (asks to be configured)
configured = False
while not configured:
    for obj, op, body in events(5):
        if obj == xsurf and op == 0:
            serial, = struct.unpack_from("<I", body)
            req(xsurf, 4, struct.pack("<I", serial))
            configured = True
        elif obj == wm and op == 0:
            req(wm, 3, body[:4])
        elif obj == 1 and op == 0:
            raise SystemExit("dmabuf-test: protocol error " + repr(body))
params = new(); req(dmabuf, 1, struct.pack("<I", params))
req(params, 1, struct.pack("<IIIII", 0, 0, STRIDE, 0, 0), fds=(fd,))   # add: plane 0, offset 0, LINEAR
buf = new(); req(params, 3, struct.pack("<IiiII", buf, W, H, XR24, 0))  # create_immed
req(params, 0)
req(surface, 1, struct.pack("<Iii", buf, 0, 0))   # attach
req(surface, 9, struct.pack("<iiii", 0, 0, W, H)) # damage_buffer
req(surface, 6)
log("committed a", W, "x", H, mode, "buffer")
end = time.time() + seconds
frame = 0
releases = 0
while time.time() < end:
    for obj, op, body in events(0.5):
        if obj == wm and op == 0:
            req(wm, 3, body[:4])
        elif obj == buf and op == 0:
            releases += 1
        elif obj == 1 and op == 0:
            oid, code = struct.unpack_from("<II", body)
            raise SystemExit("dmabuf-test: protocol error on %d code %d: %r" % (oid, code, body[12:]))
    if mode == "memfd" and frame < 5:
        frame += 1
        picture(fd, frame)
        req(surface, 1, struct.pack("<Iii", buf, 0, 0))
        req(surface, 9, struct.pack("<iiii", 0, 0, W, H))
        req(surface, 6)
log("done:", frame, "more frames,", releases, "releases")
