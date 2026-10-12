#!/usr/bin/env python3
# A Wayland client in plain Python for testing wlserver's Plasma protocols without plasmashell:
#   wl-plasma-test.py panel      a layer surface like plasmashell's panel (top layer, bottom edge, exclusive zone 40),
#                        then the task manager's view (org_kde_plasma_window_management) of the windows, and
#                        its requests: activate, minimize, maximize; virtual desktops; prints what it sees.
# The protocol's opcodes are read from the server's own tables (/run/wlserver/protocol.js).
import os, re, socket, struct, sys, time, mmap

PROTO = open("/run/wlserver/protocol.js").read()
IFACES = {}
for m in re.finditer(r"\n    ([a-z_0-9]+): \{\n      version: (\d+),\n      requests: \[\n(.*?)      \],\n      events: \[\n(.*?)      \],", PROTO, re.S):
    req = [json for json in re.findall(r'"name":"([a-zA-Z_0-9]+)","sig":"([^"]*)"', m.group(3))]
    ev = [json for json in re.findall(r'"name":"([a-zA-Z_0-9]+)","sig":"([^"]*)"', m.group(4))]
    IFACES[m.group(1)] = {"req": {n: (i, s) for i, (n, s) in enumerate(req)}, "ev": ev}

def say(*a):
    print("wltest:", *a, flush=True)

class Conn:
    def __init__(self):
        path = os.environ["XDG_RUNTIME_DIR"] + "/" + os.environ.get("WAYLAND_DISPLAY", "wayland-0")
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.next = 2
        self.objs = {1: "wl_display"}
        self.handlers = {}
        self.buf = b""
        self.fds = []

    def new(self, iface):
        i = self.next
        self.next += 1
        self.objs[i] = iface
        return i

    def req(self, oid, name, *args, fds=()):
        op, sig = IFACES[self.objs[oid]]["req"][name]
        body = b""
        it = iter(args)
        for c in sig.replace("?", ""):
            if c == "h":
                continue
            v = next(it)
            if c in "uon":
                body += struct.pack("<I", v or 0)
            elif c == "i":
                body += struct.pack("<i", v)
            elif c == "f":
                body += struct.pack("<i", int(v * 256))
            elif c == "s":
                if v is None:
                    body += struct.pack("<I", 0)
                else:
                    b = v.encode() + b"\0"
                    body += struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)
            elif c == "a":
                body += struct.pack("<I", len(v)) + v + b"\0" * (-len(v) % 4)
        msg = struct.pack("<II", oid, ((8 + len(body)) << 16) | op) + body
        if fds:
            socket.send_fds(self.s, [msg], list(fds))
        else:
            self.s.sendall(msg)

    def bind(self, name, iface, version):
        oid = self.new(iface)
        n = iface.encode() + b"\0"
        body = struct.pack("<I", name) + struct.pack("<I", len(n)) + n + b"\0" * (-len(n) % 4) + struct.pack("<II", version, oid)
        self.s.sendall(struct.pack("<II", self.registry, ((8 + len(body)) << 16) | 0) + body)
        return oid

    def pump(self, timeout=0.2):
        self.s.settimeout(timeout)
        try:
            data, fds, _, _ = socket.recv_fds(self.s, 65536, 16)
        except socket.timeout:
            return False
        if not data:
            raise SystemExit("wltest: the server closed the connection")
        self.buf += data
        self.fds += fds
        while len(self.buf) >= 8:
            oid, word = struct.unpack_from("<II", self.buf)
            size, op = word >> 16, word & 0xffff
            if len(self.buf) < size:
                break
            body, self.buf = self.buf[8:size], self.buf[size:]
            iface = self.objs.get(oid)
            if not iface:
                continue
            name, sig = IFACES[iface]["ev"][op]
            args, p = [], 0
            for c in sig.replace("?", ""):
                if c == "h":
                    args.append(self.fds.pop(0))
                    continue
                if c in "uon":
                    args.append(struct.unpack_from("<I", body, p)[0]); p += 4
                elif c == "i":
                    args.append(struct.unpack_from("<i", body, p)[0]); p += 4
                elif c == "f":
                    args.append(struct.unpack_from("<i", body, p)[0] / 256); p += 4
                elif c in "sa":
                    n = struct.unpack_from("<I", body, p)[0]; p += 4
                    v = body[p:p + n]; p += n + (-n % 4)
                    args.append(v[:-1].decode() if c == "s" and n else (None if c == "s" else v))
            if iface == "wl_display" and name == "error":
                raise SystemExit("wltest: protocol error: object %d code %d: %s" % tuple(args))
            if iface == "wl_display" and name == "delete_id":
                self.objs.pop(args[0], None)
                continue
            h = self.handlers.get((oid, name)) or self.handlers.get((iface, name))
            if h:
                h(oid, *args)
        return True

    def wait(self, cond, seconds, what):
        end = time.time() + seconds
        while not cond():
            if time.time() > end:
                raise SystemExit("wltest: no " + what + " after %d s" % seconds)
            self.pump(0.5)

def main():
    c = Conn()
    c.registry = c.new("wl_registry")
    c.req(1, "get_registry", c.registry)
    globals_ = {}
    c.handlers[(c.registry, "global")] = lambda o, n, i, v: globals_.__setitem__(i, (n, v))
    done = []
    cb = c.new("wl_callback")
    c.handlers[(cb, "done")] = lambda o, d: done.append(1)
    c.req(1, "sync", cb)
    c.wait(lambda: done, 300, "registry")
    need = ["wl_compositor", "wl_shm", "zwlr_layer_shell_v1", "org_kde_plasma_window_management",
            "org_kde_plasma_virtual_desktop_management", "org_kde_plasma_shell", "kde_output_order_v1"]
    say("globals offered:", " ".join(i for i in need if i in globals_), "| missing:", " ".join(i for i in need if i not in globals_))
    compositor = c.bind(globals_["wl_compositor"][0], "wl_compositor", 4)
    shm = c.bind(globals_["wl_shm"][0], "wl_shm", 1)
    layer_shell = c.bind(globals_["zwlr_layer_shell_v1"][0], "zwlr_layer_shell_v1", 4)

    # ---- the panel: a layer surface on the bottom edge
    surface = c.new("wl_surface")
    c.req(compositor, "create_surface", surface)
    ls = c.new("zwlr_layer_surface_v1")
    c.req(layer_shell, "get_layer_surface", ls, surface, 0, 2, "dock")
    c.req(ls, "set_anchor", 2 | 4 | 8)
    c.req(ls, "set_size", 0, 40)
    c.req(ls, "set_exclusive_zone", 40)
    c.req(surface, "commit")
    conf = []
    c.handlers[(ls, "configure")] = lambda o, serial, w, h: conf.append((serial, w, h))
    c.wait(lambda: conf, 300, "layer surface configure")
    serial, w, h = conf[-1]
    say("panel configured: %dx%d" % (w, h))
    c.req(ls, "ack_configure", serial)
    fd = os.memfd_create("panel")
    os.ftruncate(fd, w * h * 4)
    m = mmap.mmap(fd, w * h * 4)
    m[:] = bytes([0x30, 0x60, 0x90, 0xff]) * (w * h)
    pool = c.new("wl_shm_pool")
    c.req(shm, "create_pool", pool, w * h * 4, fds=[fd])
    buf = c.new("wl_buffer")
    c.req(pool, "create_buffer", buf, 0, w, h, w * 4, 0)
    c.req(surface, "attach", buf, 0, 0)
    c.req(surface, "damage", 0, 0, w, h)
    c.req(surface, "commit")
    c.pump(1)

    # ---- the task manager's view
    wm = c.bind(globals_["org_kde_plasma_window_management"][0], "org_kde_plasma_window_management", 17)
    wins = {}		# object id -> dict
    order = []

    def created(o, wid, uuid):
        po = c.new("org_kde_plasma_window")
        wins[po] = {"uuid": uuid, "title": "", "app": "", "state": 0, "geometry": None, "client": None, "initial": False,
                    "desktops": set(), "unmapped": False}
        c.req(wm, "get_window_by_uuid", po, uuid)
    c.handlers[(wm, "window_with_uuid")] = created
    def field(name, key, conv=lambda *a: a[0]):
        c.handlers[("org_kde_plasma_window", name)] = lambda o, *a: wins[o].__setitem__(key, conv(*a))
    field("title_changed", "title")
    field("app_id_changed", "app")
    field("state_changed", "state")
    field("themed_icon_name_changed", "icon")
    field("pid_changed", "pid")
    field("geometry", "geometry", lambda *a: a)
    field("client_geometry", "client", lambda *a: a)
    field("initial_state", "initial", lambda *a: True)
    field("unmapped", "unmapped", lambda *a: True)
    c.handlers[("org_kde_plasma_window", "virtual_desktop_entered")] = lambda o, d: wins[o]["desktops"].add(d)
    c.handlers[("org_kde_plasma_window", "virtual_desktop_left")] = lambda o, d: wins[o]["desktops"].discard(d)
    c.handlers[(wm, "stacking_order_changed_2")] = lambda o: get_order()
    def get_order():
        so = c.new("org_kde_plasma_stacking_order")
        got = []
        c.handlers[(so, "window")] = lambda o, u: got.append(u)
        c.handlers[(so, "done")] = lambda o: order.__setitem__(slice(None), got)
        c.req(wm, "get_stacking_order", so)

    live = lambda: [w for w in wins.values() if w["initial"] and not w["unmapped"]]
    c.wait(lambda: len(live()) >= 2, 600, "two windows in the task manager")
    for _ in range(4):
        c.pump(0.5)
    def show(label):
        say(label)
        for w in live():
            st = w["state"]
            flags = ",".join(n for b, n in [(1, "active"), (2, "minimized"), (4, "maximized"), (0x1000, "skiptaskbar")] if st & b)
            say("  %s %r app %r icon %r state 0x%x (%s) frame %s client %s desktops %d pid %s" % (
                w["uuid"][:9], w["title"], w["app"], w.get("icon"), st, flags, w["geometry"], w["client"], len(w["desktops"]), w.get("pid")))
        say("  stacking order: " + " ".join(u[:9] for u in order))
    show("windows:")
    po = {w["uuid"]: o for o, w in wins.items()}
    a, b = sorted(live(), key=lambda w: order.index(w["uuid"]) if w["uuid"] in order else -1)[:2]
    # The one below is activated, as a click on its task button does (WaylandTasksModel::requestActivate).
    c.req(po[a["uuid"]], "set_state", 1, 1)
    c.wait(lambda: a["state"] & 1 and not b["state"] & 1, 300, "activation")
    for _ in range(3):
        c.pump(0.3)
    show("after set_state(active) on the lower one:")
    # Minimized (a click on the active window's button), then back.
    c.req(po[a["uuid"]], "set_state", 2, 2)
    c.wait(lambda: a["state"] & 2, 300, "minimize")
    show("after set_state(minimized):")
    c.req(po[a["uuid"]], "set_state", 2 | 1, 1)
    c.wait(lambda: not a["state"] & 2 and a["state"] & 1, 300, "unminimize")
    show("after set_state(active) of the minimized one:")
    # Maximized: it has to end above the panel's exclusive zone.
    c.req(po[a["uuid"]], "set_state", 4, 4)
    c.wait(lambda: a["state"] & 4, 300, "maximize")
    end = time.time() + 30
    while time.time() < end:
        c.pump(0.5)
        g = a["client"]
        if g and g[1] + g[3] <= h_screen - 40 and g[3] > 300:
            break
    show("after set_state(maximized):")

    # ---- virtual desktops
    vdm = c.bind(globals_["org_kde_plasma_virtual_desktop_management"][0], "org_kde_plasma_virtual_desktop_management", 2)
    desks, vdone = [], []
    c.handlers[(vdm, "desktop_created")] = lambda o, i, p: desks.insert(p, i)
    c.handlers[(vdm, "desktop_removed")] = lambda o, i: desks.remove(i)
    c.handlers[(vdm, "rows")] = lambda o, r: None
    c.handlers[(vdm, "done")] = lambda o: vdone.append(1)
    c.wait(lambda: vdone, 300, "virtual desktops")
    say("virtual desktops:", len(desks))
    c.req(vdm, "request_create_virtual_desktop", "Second", len(desks))
    n0 = len(desks)
    c.wait(lambda: len(desks) > n0, 300, "a new desktop")
    vd = c.new("org_kde_plasma_virtual_desktop")
    names, act = [], []
    c.handlers[(vd, "name")] = lambda o, n: names.append(n)
    c.handlers[(vd, "activated")] = lambda o: act.append(1)
    c.handlers[(vd, "desktop_id")] = lambda o, i: None
    c.handlers[(vd, "done")] = lambda o: None
    c.req(vdm, "get_virtual_desktop", vd, desks[-1])
    c.pump(1)
    say("virtual desktops now:", len(desks), "the new one is", names)
    c.req(vd, "request_activate")
    c.wait(lambda: act, 300, "the new desktop activated")
    say("the new desktop is the current one")
    c.req(po[b["uuid"]], "request_enter_virtual_desktop", desks[-1])
    c.wait(lambda: desks[-1] in b["desktops"], 300, "a window on the new desktop")
    say("the other window is on the new desktop now")
    say("ok")

h_screen = int(os.environ.get("SCREEN_H", "768"))
main()
