#!/usr/bin/env python3
# The page's server in the run image (docker-compose.yml, service www): what linux-wasm's runtime/server.py does
# (Cross-Origin-Opener-Policy and -Embedder-Policy, which the page needs for shared memory; no caching: the page's
# files change with every build, and the disks are kept by the page itself, in the browser's private file system),
# plus:
#   - several requests at once (the disk worker fetches pieces while the page loads its scripts), HTTP/1.1;
#   - DIST/www as the site, DIST/disk as its disk/ (tools/pack-www.sh's layout);
#   - / is the landing page (SITE/index.html: site/ in the tree, with robots.txt, sitemap.xml and the icons), whose
#     Start button goes to /desktop; /desktop is dev.html, the machine, which boots at once (noindex; the query is
#     kept: /desktop?autostart=0). Without a SITE directory, / goes to dev.html as before;
#   - /rv-site.json: the defaults dev.html takes from the server: the relay's port as the browser reaches it
#     (--net-port) and the disk when the address names none (--disk).
#
#   serve.py --dist DIST [--bind ADDRESS] [--port PORT] [--net-port PORT] [--disk NAME] [--site SITE]
import argparse
import http.server
import json
import os
import posixpath
import sys
import urllib.parse


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dist", default="/dist")
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--net-port", type=int, default=8001)
    p.add_argument("--disk", default="release")
    p.add_argument("--site", default="/site", help="the landing page's files (site/ in the tree)")
    p.add_argument("--dns", default="", help="the resolver the machine uses (rv-site.json), e.g. 10.0.5.1")
    a = p.parse_args()
    www = os.path.join(a.dist, "www")
    disks = os.path.join(a.dist, "disk")
    if not os.path.exists(os.path.join(www, "dev.html")):
        sys.exit(f"serve.py: no {www}/dev.html (build the dist: docker/release.sh, or set RV_RELEASE_URL)")
    landing = os.path.isfile(os.path.join(a.site, "index.html"))
    site = json.dumps({"net_port": a.net_port, "disk": a.disk, **({"dns": a.dns} if a.dns else {})}).encode()

    class Handler(http.server.SimpleHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        extensions_map = {
            **http.server.SimpleHTTPRequestHandler.extensions_map,
            ".wasm": "application/wasm",
            ".js": "text/javascript",
            ".mjs": "text/javascript",
            ".json": "application/json",
        }

        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=www, **kwargs)

        def end_headers(self):
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cache-Control", "no-store")
            if self.path.split("?", 1)[0].rstrip("/") in ("/desktop", "/dev.html"):
                self.send_header("X-Robots-Tag", "noindex, nofollow")
            super().end_headers()

        def translate_path(self, path):
            clean = posixpath.normpath(urllib.parse.unquote(path.split("?", 1)[0].split("#", 1)[0]))
            if clean == "/disk" or clean.startswith("/disk/"):
                rest = [s for s in clean[len("/disk"):].split("/") if s and s not in (".", "..")]
                return os.path.join(disks, *rest)
            if clean == "/desktop":
                return os.path.join(www, "dev.html")
            if landing:
                mine = os.path.join(a.site, *[s for s in clean.split("/") if s and s not in (".", "..")])
                if clean == "/":
                    mine = os.path.join(a.site, "index.html")
                if os.path.isfile(mine):
                    return mine
            return super().translate_path(path)

        def do_GET(self):
            if self.special():
                return
            super().do_GET()

        def do_HEAD(self):
            if self.special(head=True):
                return
            super().do_HEAD()

        def special(self, head=False):
            url = urllib.parse.urlsplit(self.path)
            if url.path == "/desktop/":
                self.send_response(302)
                self.send_header("Location", "/desktop" + ("?" + url.query if url.query else ""))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return True
            if not landing and url.path in ("/", "/index.html"):
                self.send_response(302)
                self.send_header("Location", "/dev.html" + ("?" + url.query if url.query else ""))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return True
            if url.path == "/rv-site.json":
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(site)))
                self.end_headers()
                if not head:
                    self.wfile.write(site)
                return True
            return False

        def log_message(self, format, *args):
            # Errors only: the disk is thousands of pieces.
            if len(args) >= 2 and str(args[1]).startswith(("2", "3")):
                return
            super().log_message(format, *args)

    server = http.server.ThreadingHTTPServer((a.bind, a.port), Handler)
    server.daemon_threads = True
    print(f"serve.py: {a.dist} on {a.bind}:{a.port} (relay port {a.net_port}, disk {a.disk}"
          + (f", landing page {a.site}" if landing else "") + ")", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
