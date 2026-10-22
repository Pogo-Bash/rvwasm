#!/usr/bin/env python3
"""serve-site.py PORT SITE DIST: tests/panelcache/serve.py's server (the live site's headers) for a site directory made from the release dist, plus
the disk pieces the dist does not hold (the home disk's: only in the bucket): a /disk/NAME/NNNNNN that is not a file is fetched, by the hash
the dist's manifest (DIST/disk/NAME/manifest.json) lists for it, from the bucket (https://disk.plasmaontheweb.com/disk/HASH.bin) and cached under
SITE/../site-cache/. Localhost only."""
import http.server, json, os, sys, urllib.request, socketserver, functools, importlib.util
port, site, dist = int(sys.argv[1]), sys.argv[2], sys.argv[3]
spec = importlib.util.spec_from_file_location("serve", os.path.expanduser("~/dev/rvwasm-wt/panelcache/tests/panelcache/serve.py"))
serve = importlib.util.module_from_spec(spec); spec.loader.exec_module(serve)
cache = os.path.join(os.path.dirname(os.path.abspath(site)), "site-cache"); os.makedirs(cache, exist_ok=True)
manifests = {}
def hashes(name):
    if name not in manifests:
        manifests[name] = json.load(open(os.path.join(dist, "disk", name, "manifest.json"))).get("hashes", [])
    return manifests[name]
class H(serve.Handler):
    def do_GET(self):
        parts = self.path.split("?")[0].strip("/").split("/")
        if len(parts) == 3 and parts[0] == "disk" and parts[2].isdigit():
            local = os.path.join(site, "disk", parts[1], parts[2])
            if not os.path.exists(local):
                h = hashes(parts[1])
                i = int(parts[2])
                if i < len(h) and h[i]:
                    c = os.path.join(cache, h[i] + ".bin")
                    if not os.path.exists(c):
                        req = urllib.request.Request("https://disk.plasmaontheweb.com/disk/" + h[i] + ".bin", headers={"User-Agent": "curl/8.4"})
                        with urllib.request.urlopen(req, timeout=120) as r, open(c + ".tmp", "wb") as f:
                            f.write(r.read())
                        os.replace(c + ".tmp", c)
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(os.path.getsize(c)))
                    self.end_headers()
                    self.wfile.write(open(c, "rb").read())
                    return
        super().do_GET()
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True; request_queue_size = 64
S(("127.0.0.1", port), functools.partial(H, directory=site)).serve_forever()
