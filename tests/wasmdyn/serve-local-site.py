#!/usr/bin/env python3
"""serve-local-site.py PORT DIST: a local dist as the live site serves it, for the browser gates (localhost only, nothing leaves the Mac
but the Debian disk's pieces, fetched once from the public bucket and kept).

  - DIST/www/* are the site's files, with the live headers (COOP/COEP, application/wasm, no-cache for the page and runtime, immutable for
    /dl/*.wasm and /disk/); /desktop is dev.html; /rv-site.json says disk=release.
  - /disk/NAME/manifest.json is DIST/disk/NAME/manifest.json with "base" pointed at /disk/ (so the page's fetcher pool runs, as it does
    against the bucket: the live manifests name the bucket, a local nqs disk has no bucket to name).
  - /disk/HASH.bin is a piece by content: from the dist's own pieces (DIST/disk/NAME/NNNNNN, indexed by the manifests' hashes) if it has
    it, else from the public bucket (https://disk.plasmaontheweb.com/disk/HASH.bin), kept in CACHE (default ~/dev/rvwasm-wt/site-cache).
    /disk/NAME/NNNNNN (a page that fetches by number) is the same piece looked up by its hash.
  - RATE (bytes per second, env): the pieces of the disks in PACE_DISKS (default "nqs": the native programs' libraries) and every
    /dl/ file share this much bandwidth, as one visitor connection would. Debian's boot pieces are not paced (the gates are about what
    the native programs add). Unset or 0: as fast as the loopback goes.
  - RELAY=HOST:PORT (env, e.g. localhost:8001 = the build VM's dev relay, tools/net-relay.py): /relay/... is passed to it (GET /relay/address and the
    WebSocket), with the Origin header it accepts (RELAY_ORIGIN, default http://localhost:8000 = the dev server's), so that the guest has a
    network (the dev relay is open: the real internet) and no dns= (the dev relay does not answer DNS). Without RELAY the page has none.
    While the file RELAY_DROP_FILE (default /tmp/rv-relay-drop) exists the relay is down for the page (open WebSockets closed, new ones refused).
  - SITE_LOG=FILE: one line per paced or fetched response: epoch seconds, class (disk-nqs, disk-release, dl, other), name, bytes.
    /__stats answers the cumulative bytes per class as JSON."""
import functools
import http.server
import json
import os
import select
import socket
import socketserver
import sys
import threading
import time
import urllib.request

port, dist = int(sys.argv[1]), os.path.abspath(sys.argv[2])
www = os.path.join(dist, "www")
RATE = float(os.environ.get("RATE", "0"))
PACE = set((os.environ.get("PACE_DISKS", "nqs")).split(","))
CACHE = os.environ.get("SITE_CACHE", os.path.expanduser("~/dev/rvwasm-wt/site-cache"))
RELAY = os.environ.get("RELAY")
RELAY_ORIGIN = os.environ.get("RELAY_ORIGIN", "http://localhost:8000")
# While this file exists the relay is "down" for the page: open WebSockets are closed, new ones refused (a gate that needs to lose and get back the relay: touch/rm it).
RELAY_DROP_FILE = os.environ.get("RELAY_DROP_FILE", "/tmp/rv-relay-drop")
LOG = open(os.environ["SITE_LOG"], "a", buffering=1) if os.environ.get("SITE_LOG") else None
os.makedirs(CACHE, exist_ok=True)

_lock = threading.Lock()
_next = [0.0]
_stats = {}


def pace(n):
    if RATE <= 0:
        return
    with _lock:
        t = max(time.time(), _next[0])
        _next[0] = t + n / RATE
    time.sleep(max(0.0, t - time.time()))


def count(kind, name, n):
    with _lock:
        _stats[kind] = _stats.get(kind, 0) + n
    if LOG:
        LOG.write("%.3f %s %s %d\n" % (time.time(), kind, name, n))


manifests = {}   # disk name -> manifest
by_hash = {}     # hash -> (disk name, local path or None)
for name in sorted(os.listdir(os.path.join(dist, "disk"))):
    m = json.load(open(os.path.join(dist, "disk", name, "manifest.json")))
    manifests[name] = m
    for i, h in enumerate(m.get("hashes", [])):
        if not h:
            continue
        local = os.path.join(dist, "disk", name, "%06d" % i)
        by_hash.setdefault(h, (name, local if os.path.exists(local) else None))
        if os.path.exists(local):
            by_hash[h] = (name, local)


# The pieces in a manifest's `warm` list (what the native programs' first starts read, besides the disks in PACE_DISKS) are paced too
warm_hashes = set()
for _name, _m in manifests.items():
    for _i in _m.get("warm", []):
        if _i < len(_m.get("hashes", [])) and _m["hashes"][_i]:
            warm_hashes.add(_m["hashes"][_i])


class H(http.server.SimpleHTTPRequestHandler):
    extensions_map = dict(http.server.SimpleHTTPRequestHandler.extensions_map,
                          **{".wasm": "application/wasm", ".js": "text/javascript", ".json": "application/json"})

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        p = self.path.split("?")[0]
        if (p.startswith("/dl/") and p.endswith(".wasm")) or (p.startswith("/disk/") and not p.endswith("manifest.json")):
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
        else:
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def log_message(self, *args):
        pass

    def translate_path(self, path):
        p = path.split("?")[0].split("#")[0]
        if p in ("/", "/desktop", "/desktop/"):
            p = "/dev.html"
        return os.path.join(www, os.path.normpath(p).lstrip("/"))

    def body(self, data, ctype="application/octet-stream", kind=None, name="", paced=False):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            for i in range(0, len(data), 65536):
                chunk = data[i:i + 65536]
                if paced or kind == "dl":
                    pace(len(chunk))
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            return
        if kind:
            count(kind, name, len(data))

    def piece(self, h):
        name, local = by_hash.get(h, (None, None))
        if local:
            return open(local, "rb").read(), name, name in PACE or h in warm_hashes
        c = os.path.join(CACHE, h + ".bin")
        if not os.path.exists(c):
            req = urllib.request.Request("https://disk.plasmaontheweb.com/disk/" + h + ".bin", headers={"User-Agent": "curl/8.4"})
            with urllib.request.urlopen(req, timeout=120) as r, open(c + ".tmp", "wb") as f:
                f.write(r.read())
            os.replace(c + ".tmp", c)
        return open(c, "rb").read(), name or "bucket", h in warm_hashes

    def relay(self):
        """/relay/PATH -> the dev relay's /PATH (a plain GET or the WebSocket upgrade), the bytes passed both ways until either side closes."""
        host, rport = RELAY.rsplit(":", 1)
        if os.path.exists(RELAY_DROP_FILE):
            self.send_error(503, "relay: dropped by %s" % RELAY_DROP_FILE)
            return
        try:
            up = socket.create_connection((host, int(rport)), timeout=10)
        except OSError as error:
            self.send_error(502, "relay: %s" % error)
            return
        path = self.path[len("/relay"):]
        if not path.startswith("/"):
            path = "/" + path
        lines = ["GET %s HTTP/1.1" % path]
        for k, v in self.headers.items():
            if k.lower() == "origin":
                v = RELAY_ORIGIN
            elif k.lower() == "host":
                v = "%s:%s" % (host, rport)
            lines.append("%s: %s" % (k, v))
        if not any(l.lower().startswith("origin:") for l in lines):
            lines.append("Origin: " + RELAY_ORIGIN)   # (a plain GET of the page's own address carries none; the dev relay wants one)
        up.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        up.settimeout(None)
        self.close_connection = True
        a = self.connection
        try:
            while True:
                r, _, _ = select.select([a, up], [], [], 1)
                if os.path.exists(RELAY_DROP_FILE):
                    return
                if not r:
                    continue
                for src, dst in ((a, up), (up, a)):
                    if src in r:
                        data = src.recv(65536)
                        if not data:
                            return
                        dst.sendall(data)
        except OSError:
            pass
        finally:
            up.close()

    def do_GET(self):
        p = self.path.split("?")[0]
        if RELAY and (p == "/relay" or p.startswith("/relay/")):
            return self.relay()
        if p == "/__stats":
            with _lock:
                return self.body(json.dumps(_stats).encode(), "application/json")
        if p == "/rv-site.json":
            site = {"net_path": "/relay", "dns": "10.0.5.1", "disk": "release"}
            if RELAY:
                del site["dns"]   # (the dev relay is no resolver: the machine keeps the one /etc/resolv.conf was made with)
            return self.body(json.dumps(site).encode(), "application/json")
        parts = p.strip("/").split("/")
        if parts[0] == "disk" and len(parts) == 3 and parts[1] in manifests:
            if parts[2] == "manifest.json":
                m = dict(manifests[parts[1]])
                if m.get("hashes"):
                    m["base"] = "/disk/"
                return self.body(json.dumps(m).encode(), "application/json")
            if parts[2].isdigit():
                hashes = manifests[parts[1]].get("hashes", [])
                i = int(parts[2])
                if i < len(hashes) and hashes[i]:
                    p = "/disk/" + hashes[i] + ".bin"
                    parts = p.strip("/").split("/")
        if parts[0] == "disk" and len(parts) == 2 and parts[1].endswith(".bin"):
            try:
                data, name, paced = self.piece(parts[1][:-4])
            except Exception as error:
                self.send_error(502, str(error))
                return
            return self.body(data, kind="disk-" + str(name), name=parts[1][:8], paced=paced)
        if parts[0] == "dl" and p.endswith(".wasm") and os.path.exists(self.translate_path(p)):
            return self.body(open(self.translate_path(p), "rb").read(), "application/wasm", kind="dl", name=os.path.basename(p))
        super().do_GET()


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    request_queue_size = 128


print("serving %s on 127.0.0.1:%d (rate %s)" % (dist, port, RATE or "unlimited"), flush=True)
S(("127.0.0.1", port), H).serve_forever()
