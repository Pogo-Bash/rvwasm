#!/usr/bin/env python3
# How a GPU window's frames reach the page, measured from the guest (T7, gpu/webgpu/INTERFACES.md 9).
# A Wayland client that draws every frame on the GPU (a virgl CLEAR into a resource made at the GPU server
# over vtest: gpu/server/, VTEST_SOCKET_NAME) and presents it one of two ways:
#   present-test.py dmabuf [SECONDS] [WxH]    stage B: the resource's backing file handed to the window server
#                                             as a zwp_linux_dmabuf_v1 buffer; the page draws the resource's
#                                             texture (slot format 2). No readback.
#   present-test.py shm [SECONDS] [WxH]       stage A, what Mesa's virgl (virpipe) does on eglSwapBuffers:
#                                             TRANSFER_GET of the resource into its backing, wait for it, copy
#                                             the backing into a wl_shm buffer, commit.
# Two buffers, each frame paced by a frame callback. Prints frames per second, the time from wl_surface.commit
# to the frame callback (the page has shown the frame), and how long the GPU side of a frame took (submit +
# busy wait, plus the readback for shm). Every 8th frame's colour (and the white band over its top rows) is
# checked in the shm mode (the readback).
# Speaks the protocols by hand (no libwayland, no Mesa), like dmabuf-test.py.
import array, mmap, os, socket, struct, sys, time

mode = sys.argv[1] if len(sys.argv) > 1 else "dmabuf"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10
W, H = (int(v) for v in (sys.argv[3] if len(sys.argv) > 3 else "300x300").split("x"))
STRIDE = W * 4
XR24 = 0x34325258
if mode not in ("dmabuf", "shm"):
    raise SystemExit("present-test: mode is dmabuf or shm")

def log(*a):
    print("present-test:", *a, flush=True)

# ---- vtest (virglrenderer's vtest_protocol.h, version 3) and virgl commands (virgl_protocol.h)
VCMD_RESOURCE_UNREF, VCMD_SUBMIT_CMD, VCMD_RESOURCE_BUSY_WAIT, VCMD_CREATE_RENDERER = 3, 6, 7, 8
VCMD_GET_CAPS2, VCMD_PING, VCMD_PROTOCOL_VERSION, VCMD_RESOURCE_CREATE2, VCMD_TRANSFER_GET2 = 9, 10, 11, 12, 13
VCMD_TRANSFER_PUT2 = 14
BUSY_WAIT_FLAG_WAIT = 1
CCMD_CREATE_OBJECT, CCMD_SET_FRAMEBUFFER_STATE, CCMD_CLEAR, CCMD_SET_SUB_CTX, CCMD_CREATE_SUB_CTX = 1, 5, 7, 28, 29
OBJ_SURFACE = 8
PIPE_CLEAR_COLOR0 = 4
FORMAT_B8G8R8X8_UNORM, PIPE_TEXTURE_2D = 2, 2
BIND_RENDER_TARGET, BIND_SAMPLER_VIEW, BIND_DISPLAY_TARGET = 1 << 1, 1 << 3, 1 << 7

def cmd0(cmd, obj, n):
    return cmd | obj << 8 | n << 16

def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]

path = os.environ.get("VTEST_SOCKET_NAME", os.environ.get("XDG_RUNTIME_DIR", "/tmp") + "/rvgpu-0")
vt = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
vt.connect(path)
def vsend(cmd, words=(), raw=None):
    payload = raw if raw is not None else struct.pack("<%dI" % len(words), *words)
    length = len(payload) if cmd == VCMD_CREATE_RENDERER else len(payload) // 4
    vt.sendall(struct.pack("<II", length, cmd) + payload)
def vrecv(n):
    b = b""
    while len(b) < n:
        c = vt.recv(n - len(b))
        if not c:
            raise SystemExit("present-test: the GPU server closed the connection")
        b += c
    return b
vsend(VCMD_CREATE_RENDERER, raw=b"present-test\0")
vsend(VCMD_PING); vsend(VCMD_RESOURCE_BUSY_WAIT, (0, 0))
assert struct.unpack("<II", vrecv(8))[1] == VCMD_PING
l, c = struct.unpack("<II", vrecv(8)); vrecv(4 * l)
vsend(VCMD_PROTOCOL_VERSION, (3,))
vrecv(8); version, = struct.unpack("<I", vrecv(4))
vsend(VCMD_GET_CAPS2)
l, c = struct.unpack("<II", vrecv(8)); vrecv(l - 1)

def resource():
    bind = BIND_RENDER_TARGET | BIND_SAMPLER_VIEW | BIND_DISPLAY_TARGET
    vsend(VCMD_RESOURCE_CREATE2, (0, PIPE_TEXTURE_2D, FORMAT_B8G8R8X8_UNORM, bind, W, H, 1, 1, 0, 0, STRIDE * H))
    l, c = struct.unpack("<II", vrecv(8)); res, = struct.unpack("<I", vrecv(4))
    fds = array.array("i")
    msg, anc, flags, addr = vt.recvmsg(1, socket.CMSG_LEN(4))
    for level, kind, data in anc:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            fds.frombytes(data[:4])
    return res, fds[0]

def submit(words):
    vsend(VCMD_SUBMIT_CMD, words)

def busy_wait(res):
    vsend(VCMD_RESOURCE_BUSY_WAIT, (res, BUSY_WAIT_FLAG_WAIT))
    l, c = struct.unpack("<II", vrecv(8)); vrecv(4 * l)

bufs = []
for i in range(2):
    res, fd = resource()
    bufs.append({"res": res, "fd": fd, "surf": i + 1, "busy": False, "map": mmap.mmap(fd, STRIDE * H)})
# a sub-context, and a surface (render target view) of each resource
words = [cmd0(CCMD_CREATE_SUB_CTX, 0, 1), 1, cmd0(CCMD_SET_SUB_CTX, 0, 1), 1]
for b in bufs:
    words += [cmd0(CCMD_CREATE_OBJECT, OBJ_SURFACE, 5), b["surf"], b["res"], FORMAT_B8G8R8X8_UNORM, 0, 0]
submit(words)
log("vtest protocol", version, "resources", [b["res"] for b in bufs], W, "x", H, mode)

def colour(frame):
    # (a slow walk round the colour wheel, so a screenshot shows something between frames)
    t = frame / 60.0
    return [(1 + v) / 2 for v in (_sin(t), _sin(t + 2.1), _sin(t + 4.2))]
def _sin(x):
    import math
    return math.sin(x)

BAND = min(32, H // 4)
def draw(b, frame):
    r, g, bl = colour(frame)
    submit([cmd0(CCMD_SET_FRAMEBUFFER_STATE, 0, 3), 1, 0, b["surf"],
            cmd0(CCMD_CLEAR, 0, 8), PIPE_CLEAR_COLOR0, f32(r), f32(g), f32(bl), f32(1.0), 0, 0, 0])
    # a white band over the first BAND rows of the resource (the top of the window: what tells a picture
    # shown upside down), uploaded from the backing (vtest TRANSFER_PUT2: res, level, x, y, z, w, h, d,
    # data size, offset)
    b["map"][:STRIDE * BAND] = b"\xff" * (STRIDE * BAND)
    vsend(VCMD_TRANSFER_PUT2, (b["res"], 0, 0, 0, 0, W, BAND, 1, STRIDE * BAND, 0))
    return r, g, bl

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
req(1, 1, struct.pack("<I", registry))
cb = new()
req(1, 0, struct.pack("<I", cb))
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
            raise SystemExit("present-test: protocol error " + repr(body))
def bind(iface, version):
    if iface not in globals_:
        raise SystemExit("present-test: the window server offers no " + iface)
    i = new()
    req(registry, 0, struct.pack("<I", globals_[iface][0]) + string(iface) + struct.pack("<II", version, i))
    return i
compositor = bind("wl_compositor", 4)
wm = bind("xdg_wm_base", 1)
surface = new(); req(compositor, 0, struct.pack("<I", surface))
xsurf = new(); req(wm, 2, struct.pack("<II", xsurf, surface))
top = new(); req(xsurf, 1, struct.pack("<I", top))
req(top, 2, string("present-test (" + mode + ")"))
req(surface, 6)
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
            raise SystemExit("present-test: protocol error " + repr(body))

wlbuf = {}   # wl_buffer id -> our buffer
if mode == "dmabuf":
    dmabuf = bind("zwp_linux_dmabuf_v1", 3)
    for b in bufs:
        params = new(); req(dmabuf, 1, struct.pack("<I", params))
        req(params, 1, struct.pack("<IIIII", 0, 0, STRIDE, 0, 0), fds=(b["fd"],))   # plane 0, LINEAR
        b["wl"] = new(); req(params, 3, struct.pack("<IiiII", b["wl"], W, H, XR24, 0))
        req(params, 0)
        wlbuf[b["wl"]] = b
else:
    shm = bind("wl_shm", 1)
    for b in bufs:
        b["shmfd"] = os.memfd_create("present-test")
        os.ftruncate(b["shmfd"], STRIDE * H)
        b["shm"] = mmap.mmap(b["shmfd"], STRIDE * H)
        pool = new(); req(shm, 0, struct.pack("<Ii", pool, STRIDE * H), fds=(b["shmfd"],))
        b["wl"] = new(); req(pool, 0, struct.pack("<IiiiiI", b["wl"], 0, W, H, STRIDE, 1))   # XRGB8888
        req(pool, 1)
        wlbuf[b["wl"]] = b

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else 0

frame, lat, gpu, wrong, reused = 0, [], [], 0, 0
callback = None
committed = 0.0
start = time.time()
end = start + seconds
while time.time() < end:
    # a buffer the window server has let go of (wait a little for one), else the older one anyway
    b = next((x for x in bufs if not x["busy"]), None)
    t_wait = time.time() + 0.1
    while b is None and time.time() < t_wait:
        for obj, op, body in events(0.02):
            if obj in wlbuf and op == 0:
                wlbuf[obj]["busy"] = False
        b = next((x for x in bufs if not x["busy"]), None)
    if b is None:
        b = bufs[frame % 2]
        reused += 1
    t0 = time.time()
    rgb = draw(b, frame)
    if mode == "shm":
        # stage A: the frame read back into the backing (vtest TRANSFER_GET2: res, level, x, y, z, w, h, d,
        # data size, offset), waited for, then copied into the wl_shm buffer
        vsend(VCMD_TRANSFER_GET2, (b["res"], 0, 0, 0, 0, W, H, 1, STRIDE * H, 0))
        busy_wait(b["res"])
        b["shm"][:] = b["map"][:]
        if frame % 8 == 0:
            px = b["map"][(H // 2) * STRIDE + (W // 2) * 4:(H // 2) * STRIDE + (W // 2) * 4 + 4]
            want = (round(rgb[2] * 255), round(rgb[1] * 255), round(rgb[0] * 255))
            top = b["map"][(BAND // 2) * STRIDE + (W // 2) * 4:(BAND // 2) * STRIDE + (W // 2) * 4 + 3]
            if any(abs(px[i] - want[i]) > 2 for i in range(3)) or top != b"\xff\xff\xff":
                wrong += 1
                if wrong <= 3:
                    log("frame", frame, "read back", tuple(px[:3]), "wanted", want, "(B, G, R); band", tuple(top))
    else:
        busy_wait(b["res"])      # the clear is with the executor before the window server hears of the frame
    gpu.append((time.time() - t0) * 1000)
    callback = new()
    req(surface, 1, struct.pack("<Iii", b["wl"], 0, 0))      # attach
    req(surface, 9, struct.pack("<iiii", 0, 0, W, H))        # damage_buffer
    req(surface, 3, struct.pack("<I", callback))             # frame
    req(surface, 6)                                          # commit
    committed = time.time()
    b["busy"] = True
    frame += 1
    shown = False
    while not shown and time.time() < end + 2:
        for obj, op, body in events(0.5):
            if obj == callback and op == 0:
                lat.append((time.time() - committed) * 1000)
                shown = True
            elif obj in wlbuf and op == 0:
                wlbuf[obj]["busy"] = False
            elif obj == wm and op == 0:
                req(wm, 3, body[:4])
            elif obj == 1 and op == 0:
                oid, code = struct.unpack_from("<II", body)
                raise SystemExit("present-test: protocol error on %d code %d: %r" % (oid, code, body[12:]))
took = time.time() - start
log("%s %dx%d: %d frames in %.1f s = %.1f fps; commit -> frame callback p50 %.1f ms p90 %.1f ms; "
    "GPU side (submit + wait%s) p50 %.2f ms p90 %.2f ms; %d buffers reused before release%s"
    % (mode, W, H, frame, took, frame / took, pct(lat, 0.5), pct(lat, 0.9),
       " + readback + copy" if mode == "shm" else "", pct(gpu, 0.5), pct(gpu, 0.9), reused,
       "; %d of %d checked frames read back wrong" % (wrong, (frame + 7) // 8) if mode == "shm" else ""))
