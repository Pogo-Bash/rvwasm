#!/usr/bin/env python3
# A page server for the release dist with some files replaced (the gate run of gpuaddr): ROOT/ first, then dist/www;
# /disk/* from dist/disk. COOP/COEP as docker/Caddyfile sends them. Range requests are served (the disk pieces are fetched so).
import os, sys, mimetypes, re
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
OVER, WWW, DISK = sys.argv[1], sys.argv[2], sys.argv[3]
port = int(sys.argv[4])
mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("text/javascript", ".js")
class H(SimpleHTTPRequestHandler):
    def translate_path(self, path):
        path = path.split("?", 1)[0].split("#", 1)[0]
        if path.startswith("/disk/"):
            return os.path.join(DISK, path[len("/disk/"):])
        rel = path.lstrip("/") or "dev.html"
        for base in (OVER, WWW):
            p = os.path.join(base, rel)
            if os.path.exists(p):
                return p
        return os.path.join(WWW, rel)
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
    def log_message(self, *a): pass
ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
