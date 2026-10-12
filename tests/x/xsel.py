#!/usr/bin/env python3
# A small X selection tool in the raw protocol (no Xlib, no xclip/xsel on the disk): for testing the window
# server's clipboard bridge (wlserver/xselection.js) from a shell in the machine, e.g. after selecting in xterm:
#   DISPLAY=:0 python3 xsel.py get PRIMARY
#   xsel.py targets SEL            the owner's TARGETS, by name
#   xsel.py get SEL [TARGET]       the selection converted to TARGET (UTF8_STRING), printed with its type
#   xsel.py own SEL TEXT [SECONDS] own SEL and answer requests (TARGETS, UTF8_STRING, STRING, TEXT) for a while
#   xsel.py owner SEL              the owner's window
# SEL: PRIMARY or CLIPBOARD. DISPLAY=:N.
import os, socket, struct, sys, time, select

disp = os.environ.get("DISPLAY", ":0").split(":")[1].split(".")[0]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/.X11-unix/X" + disp)

def recvn(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise SystemExit("connection closed")
        b += c
    return b

s.sendall(struct.pack("<BxHHHHxx", 0x6c, 11, 0, 0, 0))
head = recvn(8)
if head[0] != 1:
    raise SystemExit("setup refused")
body = recvn(struct.unpack("<H", head[6:8])[0] * 4)
rid_base, rid_mask = struct.unpack("<II", body[4:12])
vlen = struct.unpack("<H", body[16:18])[0]
nformats = body[21]
screen = 32 + ((vlen + 3) & ~3) + 8 * nformats
root = struct.unpack("<I", body[screen:screen + 4])[0]
seq = 0
pending = []  # events read while waiting for a reply

def request(op, data, minor=0):
    global seq
    pad = (-len(data)) % 4
    data += b"\0" * pad
    s.sendall(struct.pack("<BBH", op, minor, 1 + len(data) // 4) + data)
    seq = (seq + 1) & 0xffff
    return seq

def read_one():
    m = recvn(32)
    if m[0] == 1:
        extra = struct.unpack("<I", m[4:8])[0]
        m += recvn(extra * 4)
    return m

def reply(n):
    while True:
        m = read_one()
        if m[0] == 0:
            raise SystemExit("X error %d for request %d (major %d)" % (m[1], struct.unpack("<H", m[2:4])[0], m[10]))
        if m[0] == 1 and struct.unpack("<H", m[2:4])[0] == n:
            return m
        pending.append(m)

def event(timeout):
    if pending:
        return pending.pop(0)
    r, _, _ = select.select([s], [], [], timeout)
    return read_one() if r else None

def atom(name):
    b = name.encode()
    m = reply(request(16, struct.pack("<HH", len(b), 0) + b, 0))
    return struct.unpack("<I", m[8:12])[0]

def atom_name(a):
    m = reply(request(17, struct.pack("<I", a)))
    n = struct.unpack("<H", m[8:10])[0]
    return m[32:32 + n].decode()

def get_property(w, prop, delete=True):
    m = reply(request(20, struct.pack("<IIIII", w, prop, 0, 0, 1 << 22), 1 if delete else 0))
    fmt = m[1]
    typ, after, n = struct.unpack("<III", m[8:20])
    size = n * (fmt // 8) if fmt else 0
    return typ, fmt, m[32:32 + size]

def send_event(dest, ev):
    request(25, struct.pack("<II", dest, 0) + ev, 0)

def change_property(w, prop, typ, fmt, data):
    n = len(data) // (fmt // 8)
    request(18, struct.pack("<IIIBxxxI", w, prop, typ, fmt, n) + data, 0)

rid = rid_base
win = rid
# CreateWindow InputOnly 1x1, event mask PropertyChange
request(1, struct.pack("<IIhhHHHHII", win, root, -10, -10, 1, 1, 0, 2, 0, 0x800) + struct.pack("<I", 1 << 22), 0)

cmd, sel = sys.argv[1], atom(sys.argv[2])
if cmd == "owner":
    m = reply(request(23, struct.pack("<I", sel)))
    print("owner 0x%x" % struct.unpack("<I", m[8:12])[0])
elif cmd in ("get", "targets"):
    target = atom("TARGETS" if cmd == "targets" else (sys.argv[3] if len(sys.argv) > 3 else "UTF8_STRING"))
    prop = atom("XSEL_DATA")
    request(24, struct.pack("<IIIII", win, sel, target, prop, 0))
    deadline = time.time() + 10
    while time.time() < deadline:
        m = event(1)
        if m and (m[0] & 0x7f) == 31:
            p = struct.unpack("<I", m[20:24])[0]
            if not p:
                print("refused")
                sys.exit(1)
            typ, fmt, data = get_property(win, p)
            if cmd == "targets":
                print(" ".join(atom_name(a) for a in struct.unpack("<%dI" % (len(data) // 4), data)))
            else:
                print("type %s format %d: %r" % (atom_name(typ), fmt, data.decode("utf-8", "replace")))
            sys.exit(0)
    print("no answer")
    sys.exit(1)
elif cmd == "own":
    text = sys.argv[3].encode()
    secs = float(sys.argv[4]) if len(sys.argv) > 4 else 30
    request(22, struct.pack("<III", win, sel, 0))
    m = reply(request(23, struct.pack("<I", sel)))
    print("owner now 0x%x (we are 0x%x)" % (struct.unpack("<I", m[8:12])[0], win), flush=True)
    names = {atom(n): n for n in ("TARGETS", "UTF8_STRING", "STRING", "TEXT", "TIMESTAMP")}
    end = time.time() + secs
    while time.time() < end:
        m = event(0.5)
        if not m:
            continue
        code = m[0] & 0x7f
        if code == 29:
            print("lost the selection", flush=True)
            break
        if code != 30:
            continue
        t, owner, req, sl, tg, pr = struct.unpack("<IIIIII", m[4:28])
        name = names.get(tg) or atom_name(tg)
        print("request for", name, flush=True)
        if not pr:
            pr = tg
        if name == "TARGETS":
            change_property(req, pr, atom("ATOM"), 32, struct.pack("<5I", *names.keys()))
        elif name in ("UTF8_STRING", "TEXT"):
            change_property(req, pr, atom("UTF8_STRING"), 8, text)
        elif name == "STRING":
            change_property(req, pr, atom("STRING"), 8, text.decode().encode("latin-1", "replace"))
        else:
            pr = 0
        send_event(req, struct.pack("<BxHIIIII", 31, 0, t, req, sl, tg, pr) + b"\0" * 8)
        request(43, b"")  # GetInputFocus as a sync
