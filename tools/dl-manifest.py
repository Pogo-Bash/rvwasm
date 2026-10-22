#!/usr/bin/env python3
"""dl-manifest.py [--out DIR] [--preload NAME]... [--order FILE] LIB...: the library manifest the page compiles ahead
(docs/wasm-dynamic-linking.md, "Code cache across visits"; patch-runtime.py item 26, os.dl.preload_manifest).

For each LIB (a wasm side module, dylink.0 first) it computes the content key (tools/dl-hash.py: what the runtime
computes from the bytes in the program's memory), copies the file to DIR/<key>.wasm (a name that never changes
content, so the server can say "immutable" and the browser can keep the compiled code with it), and lists it in
DIR/libs.json:

    {"libs": [{"key": ..., "name": "libQt6Core.so.6", "url": "<key>.wasm", "size": N, "preload": true, "order": 12}, ...]}

"order" (with --order FILE, one library name per line): the position in the order the page compiles the libraries it did not
preload, in the background (dev.html): a library the file does not name has none (the page takes those last).

urls are relative to libs.json. "preload": false entries are listed but not compiled at page load (a library most
programs do not use costs code space for nothing); --preload NAME (a file's basename) turns it on, none given: all.
Serve DIR with `Cache-Control: public, max-age=31536000, immutable` and `Content-Type: application/wasm`.
tools/stage.sh puts $RVWASM_WORK/dl/ (this tool's output) into www/dl/; the page loads dl/libs.json with ?dl=1."""
import argparse
import importlib.util
import json
import os
import shutil
import sys

spec = importlib.util.spec_from_file_location("dl_hash", os.path.join(os.path.dirname(os.path.abspath(__file__)), "dl-hash.py"))
dl_hash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dl_hash)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="dl")
    ap.add_argument("--preload", action="append", default=[])
    ap.add_argument("--order", help="a file of library names (basenames), one per line, in the order to compile them in the background")
    ap.add_argument("libs", nargs="+")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    order = {}
    if args.order:
        for i, line in enumerate(l.strip() for l in open(args.order) if l.strip()):
            order.setdefault(line, i)
    libs = []
    for path in args.libs:
        data = open(path, "rb").read()
        if data[:4] != b"\0asm":
            sys.exit("dl-manifest: %s is not a wasm module" % path)
        key = dl_hash.key(data)
        name = os.path.basename(path)
        shutil.copyfile(path, os.path.join(args.out, key + ".wasm"))
        libs.append({"key": key, "name": name, "url": key + ".wasm", "size": len(data),
                     "preload": not args.preload or name in args.preload})
        if name in order:
            libs[-1]["order"] = order[name]
        print("%s  %s  %d bytes%s" % (key, name, len(data), "" if libs[-1]["preload"] else "  (not preloaded)"))
    with open(os.path.join(args.out, "libs.json"), "w") as f:
        json.dump({"libs": libs}, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
