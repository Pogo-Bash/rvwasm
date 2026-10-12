#!/usr/bin/env python3
"""Serve Blender for the browser (blenderweb/) to the machine's page, from another origin.

    python3 blenderweb/serve.py [--port 8040] [--bind 127.0.0.1] [--release ~/rvwasm/blender-web]

/ is blenderweb/www/ (the page that starts Blender, next to this file), /release/ the release as
fetch-release.sh unpacked it. The machine's page (http://localhost:8000) puts http://127.0.0.1:8040/ in an
iframe: another site to the browser, so the frame gets a process of its own (its own memory, its own limit on
compiled code) instead of sharing the machine's tab's.

Every answer says what an iframe of a cross-origin-isolated page needs to be isolated too (Blender's threads
need SharedArrayBuffer): Cross-Origin-Embedder-Policy require-corp, Cross-Origin-Resource-Policy cross-origin
(the machine's page, require-corp itself, may embed it), Cross-Origin-Opener-Policy same-origin (for when the
page is opened by itself). Only the machine's page, from this computer, may frame it.

Threaded: the engine is 145 MB and Blender's files 67 MB, fetched at once, and the dev server on :8000 is a
single thread that the machine itself needs.
"""
import argparse
import http.server
import os
import socketserver
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))

TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json",
    ".wasm": "application/wasm",
    ".tar": "application/x-tar",
    ".txt": "text/plain; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    release = None

    def translate_path(self, path):
        path = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        parts = [p for p in path.split("/") if p and p not in (".", "..")]
        if parts and parts[0] == "release":
            return os.path.join(self.release, *parts[1:])
        return os.path.join(HERE, "www", *parts)

    def guess_type(self, path):
        return TYPES.get(os.path.splitext(path)[1], "application/octet-stream")

    def end_headers(self):
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Content-Security-Policy",
                         "frame-ancestors 'self' http://localhost:* http://127.0.0.1:*")
        if self.path.startswith("/release/"):
            self.send_header("Cache-Control", "max-age=86400")
        else:
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(404)
        return None

    def log_message(self, format, *args):
        sys.stderr.write("%s %s\n" % (self.log_date_time_string(), format % args))


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8040)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--release", default=os.path.expanduser("~/rvwasm/blender-web"))
    a = ap.parse_args()
    if not os.path.exists(os.path.join(a.release, "blender.wasm")):
        sys.exit("serve.py: no release in %s (blenderweb/fetch-release.sh puts it there)" % a.release)
    Handler.release = a.release
    with Server((a.bind, a.port), Handler) as s:
        print("serve.py: Blender on http://%s:%d/ (release %s)" % (a.bind, a.port, a.release), flush=True)
        s.serve_forever()


if __name__ == "__main__":
    main()
