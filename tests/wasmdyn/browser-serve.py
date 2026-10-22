#!/usr/bin/env python3
"""browser-serve.py DIR [PORT]: serve the dynamic-library test machine to the owner's browser (nothing here starts one).

Files of DIR (written by browser-prepare.sh: the patched runtime, dev.html, the test initramfs, dl/ with the library
manifest and the libraries) are served as they are; everything else is fetched from the dev server (localhost:8000) and
passed on. Every response has the cross-origin isolation headers the machine needs, and dl/* is served the way a
production server should serve it for the browser's code cache: Content-Type application/wasm, Cache-Control
public, max-age=31536000, immutable (the file names are content keys). Default port 8123."""
import http.server
import os
import sys
import urllib.request

ROOT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8123
UPSTREAM = "http://localhost:8000"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?")[0].lstrip("/") or "dev.html"
        local = os.path.normpath(os.path.join(ROOT, path))
        if local.startswith(ROOT + os.sep) and os.path.isfile(local):
            data = open(local, "rb").read()
            ctype = {"html": "text/html", "js": "text/javascript", "wasm": "application/wasm", "json": "application/json",
                     "gz": "application/gzip"}.get(local.rsplit(".", 1)[-1], "application/octet-stream")
            cache = "public, max-age=31536000, immutable" if path.startswith("dl/") and path.endswith(".wasm") else "no-cache"
            self.reply(200, data, ctype, cache)
            return
        try:
            with urllib.request.urlopen(UPSTREAM + self.path) as r:
                self.reply(r.status, r.read(), r.headers.get("Content-Type", "application/octet-stream"), "no-cache")
        except Exception as error:
            self.reply(404, str(error).encode(), "text/plain", "no-cache")

    def reply(self, status, data, ctype, cache):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


if __name__ == "__main__":
    kcalc = os.path.exists(os.path.join(ROOT, "initramfs-kcalc-wasm64_nommu.cpio.gz"))
    print("http://localhost:%d/dev.html?%s" % (PORT, "image=kcalc&dl=1&disk=daily" if kcalc else "image=wasmdyn&dl=1"))
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
