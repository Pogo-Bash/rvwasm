#!/usr/bin/env python3
"""serve-dir.py DIR PORT: serve DIR on 127.0.0.1 (the library manifest for node-machine --dl-manifest). Unlike `python3 -m http.server`
the listen backlog is large: the page fetches every library at once, and with 77 of them the default 5 drops connections (ETIMEDOUT)."""
import functools
import http.server
import sys

class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 512

handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=sys.argv[1])
handler.log_message = lambda *a: None
Server(("127.0.0.1", int(sys.argv[2])), handler).serve_forever()
