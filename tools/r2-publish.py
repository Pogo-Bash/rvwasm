#!/usr/bin/env python3
"""Put a dist's disk pieces on S3-style object storage (Cloudflare R2), content-addressed, and say so in the manifest.

    r2-publish.py DIST [--base URL] [--jobs N] [--dry-run] [--disk NAME ...] [--boot-dir DIR]
    r2-publish.py DIST --boot-only [--boot-dir DIR]
    r2-publish.py DIST --dl [--dl-base URL] [--dry-run]

For every DIST/disk/NAME/ (tools/split-disk.py's pieces and manifest.json):

  1. each piece is hashed (sha256) and becomes the object  PREFIX/<sha256>.bin  (PREFIX is the path of --base:
     with --base https://disk.example.com/disk/ it is "disk/"), so that a piece never changes under its name, a
     new disk shares every piece it has in common with the old one, and a CDN may keep a piece for ever;
  2. the objects that are not in the bucket yet are uploaded (only those; the bucket is listed once), with
     "Cache-Control: public, max-age=31536000, immutable" and "Content-Type: application/octet-stream";
  3. only when every disk's pieces are all there, manifest.json is rewritten (to a new file, then renamed, so a
     hard link to the build's copy is not touched) with
         base    where the pieces are: the page fetches BASE + <sha256> + ".bin"
         hashes  one entry per piece, its sha256 in hexadecimal, "" for a piece that is all zeros
         boot    (if DIR/NAME.json exists, see below) the pieces a cold boot reads, in order: the page fetches them
                 ahead (tools/www/disk-worker.js "Fetching ahead")
     and nothing else changed (the `id` stays: browsers keep what they have of the disk).

The boot list is docker/boot/NAME.json (--boot-dir), made from a traced cold boot by tools/boot-list.py:
{"id", "boot": [piece numbers], "hashes": [their sha256]}. When its `id` is the disk's, `boot` is used as it is; when
the disk is another one (a release since), the list is carried over by hash, to the pieces that have those hashes now.
--boot-only does just that to the manifests already in DIST (no hashing of the pieces, no bucket, no credentials;
docker/patch-dist.sh does it, so that every deploy has the list).

--dl: the shared libraries of ?dl=1 (docs/wasm-dynamic-linking.md): DIST/www/dl/ holds tools/dl-manifest.py's libs.json and
the library files <key>.wasm. Each file becomes the object  PREFIX/<key>.wasm  (PREFIX is the path of --dl-base, default
https://disk.plasmaontheweb.com/dl/; Content-Type "application/wasm", which compileStreaming insists on, and the same
immutable Cache-Control: the key is a hash of the bytes, so a name never changes content). Only the missing keys are
uploaded; then, once the bucket holds all of them with the right size, libs.json is rewritten (last) so that each library's
`url` is the absolute bucket address. The files stay in DIST/www/dl too (the site's own fallback, Caddyfile zone dl).
Nothing but the dl files is touched in this mode (no disk is hashed or uploaded). Run it again and it changes nothing.

The manifest is therefore the last thing that is published: copy it to the server after the pieces are there
(docker/deploy.sh does), and the switch from the server's own pieces to the bucket is atomic.

Optional: without credentials in the environment this does nothing (exit 0) and the manifest has no `base`, so
the page fetches the pieces from the site itself (docs/deploy.md "Pieces from object storage").

Environment (the credentials go in the environment, never into arguments, files in the tree or the build VM's disk):
    R2_ENDPOINT           https://ACCOUNT.r2.cloudflarestorage.com
    R2_BUCKET             the bucket
    R2_ACCESS_KEY_ID      an S3 token that may write to the bucket
    R2_SECRET_ACCESS_KEY
    RV_PIECE_BASE         --base, if not given as an argument
Only the Python 3 standard library is used.
"""

import argparse
import concurrent.futures
import datetime
import hashlib
import hmac
import http.client
import json
import os
import ssl
import sys
import threading
import time
import urllib.parse
import xml.etree.ElementTree as ET

CACHE_CONTROL = "public, max-age=31536000, immutable"
CONTENT_TYPE = "application/octet-stream"
WASM_TYPE = "application/wasm"
DL_BASE = "https://disk.plasmaontheweb.com/dl/"
EXTENSION = ".bin"          # (Cloudflare caches by file extension unless a rule says otherwise; .bin is cached)


class Bucket:
    """The few S3 calls this needs (path style, signature version 4, region "auto"), over connections that persist
    (one per thread)."""

    def __init__(self, endpoint, bucket, key, secret):
        url = urllib.parse.urlsplit(endpoint)
        self.host = url.netloc
        self.bucket = bucket
        self.key = key
        self.secret = secret
        self.local = threading.local()

    def connection(self):
        c = getattr(self.local, "c", None)
        if c is None:
            c = self.local.c = http.client.HTTPSConnection(self.host, timeout=60, context=ssl.create_default_context())
        return c

    def sign(self, method, path, query, headers, payload_hash):
        now = datetime.datetime.now(datetime.timezone.utc)
        stamp, day = now.strftime("%Y%m%dT%H%M%SZ"), now.strftime("%Y%m%d")
        headers = dict(headers, host=self.host)
        headers["x-amz-content-sha256"] = payload_hash
        headers["x-amz-date"] = stamp
        names = sorted(k.lower() for k in headers)
        canonical_headers = "".join("%s:%s\n" % (k, str(headers[k]).strip()) for k in names)
        canonical_query = "&".join("%s=%s" % (urllib.parse.quote(k, safe="-_.~"), urllib.parse.quote(v, safe="-_.~"))
                                   for k, v in sorted(query.items()))
        request = "\n".join([method, urllib.parse.quote(path, safe="/-_.~"), canonical_query, canonical_headers,
                             ";".join(names), payload_hash])
        scope = "%s/auto/s3/aws4_request" % day
        to_sign = "\n".join(["AWS4-HMAC-SHA256", stamp, scope, hashlib.sha256(request.encode()).hexdigest()])
        k = ("AWS4" + self.secret).encode()
        for part in (day, "auto", "s3", "aws4_request"):
            k = hmac.new(k, part.encode(), hashlib.sha256).digest()
        signature = hmac.new(k, to_sign.encode(), hashlib.sha256).hexdigest()
        headers["Authorization"] = "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s" % (
            self.key, scope, ";".join(names), signature)
        headers.pop("host")
        return headers

    def call(self, method, key, query=None, body=b"", headers=None, retries=5):
        path = "/%s/%s" % (self.bucket, key) if key else "/%s" % self.bucket
        query = query or {}
        payload_hash = hashlib.sha256(body).hexdigest()
        problem = None
        for attempt in range(retries):
            try:
                h = self.sign(method, path, query, headers or {}, payload_hash)
                c = self.connection()
                target = urllib.parse.quote(path, safe="/-_.~")
                if query:
                    target += "?" + "&".join("%s=%s" % (urllib.parse.quote(k, safe="-_.~"), urllib.parse.quote(v, safe="-_.~"))
                                             for k, v in sorted(query.items()))
                c.request(method, target, body=body if body else None, headers=h)
                r = c.getresponse()
                data = r.read()
                if r.status < 300 or r.status in (403, 404):
                    return r.status, data
                problem = "%d %s" % (r.status, data[:200].decode("utf-8", "replace"))
            except (OSError, http.client.HTTPException) as e:
                problem = repr(e)
                self.local.c = None
            time.sleep(min(2 ** attempt, 20))
        raise RuntimeError("%s %s: %s" % (method, key, problem))

    def list(self, prefix):
        """{key: size} of every object under prefix."""
        found = {}
        token = None
        while True:
            query = {"list-type": "2", "prefix": prefix, "max-keys": "1000"}
            if token:
                query["continuation-token"] = token
            status, data = self.call("GET", "", query)
            if status != 200:
                raise RuntimeError("list: %d %s" % (status, data[:200]))
            root = ET.fromstring(data)
            ns = root.tag[:root.tag.index("}") + 1] if root.tag.startswith("{") else ""
            for item in root.iter(ns + "Contents"):
                found[item.find(ns + "Key").text] = int(item.find(ns + "Size").text)
            if root.findtext(ns + "IsTruncated") == "true":
                token = root.findtext(ns + "NextContinuationToken")
            else:
                return found

    def put(self, key, body, content_type=CONTENT_TYPE):
        status, data = self.call("PUT", key, body=body, headers={"content-type": content_type,
                                                                  "cache-control": CACHE_CONTROL})
        if status >= 300:
            raise RuntimeError("PUT %s: %d %s" % (key, status, data[:200]))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def boot_list(boot_dir, name, manifest, hashes, key="boot", suffix=""):
    """The `boot` list for disk `name` from boot_dir/NAME.json (see the top), or None if there is none. With key="warm", suffix="-warm":
    the pieces the native programs' first starts read (the page fetches them in the background once the desktop is up), from
    boot_dir/NAME-warm.json, the same way (tests/wasmdyn/warm-list.py makes it from a gate run's request log)."""
    path = os.path.join(boot_dir, name + suffix + ".json")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        b = json.load(f)
    if b.get("id") == manifest["id"]:
        boot = list(b[key])
    else:
        where = {}
        for i, h in enumerate(hashes):
            if h:
                where.setdefault(h, []).append(i)
        boot = [i for h in b.get("hashes", []) for i in where.get(h, [])]
        print("r2-publish: %s: the boot list is of disk %s, carried over by hash to %d of its %d pieces" % (
            name, b.get("id"), len(boot), len(b.get(key, []))))
    seen = set()
    out = []
    for i in boot:
        if isinstance(i, int) and 0 <= i < len(hashes) and hashes[i] and i not in seen:
            seen.add(i)
            out.append(i)
    return out


def write_manifest(path, m):
    tmp = path + ".part"
    with open(tmp, "w") as f:
        json.dump(m, f)
        f.write("\n")
    os.replace(tmp, path)   # (a new file: a hard link to the build's copy is not touched)


def publish_dl(a, bucket):
    """--dl: DIST/www/dl/<key>.wasm to the bucket as PREFIX/<key>.wasm, then libs.json with absolute urls (see the top)."""
    base = a.dl_base if a.dl_base.endswith("/") else a.dl_base + "/"
    prefix = urllib.parse.urlsplit(base).path.lstrip("/")
    d = os.path.join(a.dist, "www", "dl")
    path = os.path.join(d, "libs.json")
    with open(path) as f:
        m = json.load(f)
    objects = {}     # key -> (path, size)
    for lib in m["libs"]:
        name = lib["key"] + ".wasm"
        file = os.path.join(d, name)
        if not os.path.exists(file):
            print("r2-publish: libs.json lists %s but %s is not in %s" % (lib["name"], name, d), file=sys.stderr)
            return 1
        with open(file, "rb") as f:
            head = f.read(4)
        if head != b"\0asm" or os.path.getsize(file) != lib["size"]:
            print("r2-publish: %s is not the wasm module libs.json describes" % name, file=sys.stderr)
            return 1
        objects[prefix + name] = (file, lib["size"])
    print("r2-publish: dl: %d libraries (%.1f MiB) for %s" % (len(objects), sum(s for _, s in objects.values()) / 1048576, base))
    have = bucket.list(prefix)
    wrong = [k for k, (_, s) in objects.items() if k in have and have[k] != s]
    if wrong:
        print("r2-publish: %d dl objects exist with another size (e.g. %s): refusing" % (len(wrong), wrong[0]), file=sys.stderr)
        return 1
    missing = [k for k in objects if k not in have]
    print("r2-publish: dl: %d in the bucket already, %d to upload (%.1f MiB)" % (
        len(objects) - len(missing), len(missing), sum(objects[k][1] for k in missing) / 1048576))
    if a.dry_run:
        return 0

    def up(key):
        with open(objects[key][0], "rb") as f:
            bucket.put(key, f.read(), WASM_TYPE)

    with concurrent.futures.ThreadPoolExecutor(max(1, a.jobs)) as ex:
        for f in [ex.submit(up, k) for k in missing]:
            f.result()
    have = bucket.list(prefix)
    bad = [k for k, (_, s) in objects.items() if have.get(k) != s]
    if bad:
        print("r2-publish: %d dl objects are missing or the wrong size after the upload (e.g. %s): libs.json not written" % (
            len(bad), bad[0]), file=sys.stderr)
        return 1
    print("r2-publish: dl: bucket has all %d libraries, sizes match" % len(objects))
    libs = [dict(lib, url=base + lib["key"] + ".wasm") for lib in m["libs"]]
    if libs != m["libs"]:
        write_manifest(path, dict(m, libs=libs))
    print("r2-publish: dl/libs.json now points at %s" % base)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dist")
    p.add_argument("--base", default=os.environ.get("RV_PIECE_BASE", ""),
                   help="where the page will fetch pieces from, e.g. https://disk.example.com/disk/")
    p.add_argument("--jobs", type=int, default=8, help="uploads at once (8)")
    p.add_argument("--dry-run", action="store_true", help="hash and compare, upload nothing, change nothing")
    p.add_argument("--disk", action="append", help="only this disk (default: all in DIST/disk)")
    p.add_argument("--boot-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docker", "boot"),
                   help="where NAME.json, the boot list of disk NAME, is (docker/boot)")
    p.add_argument("--boot-only", action="store_true",
                   help="only put the boot lists into the manifests that are in DIST (they must have `hashes` already)")
    p.add_argument("--dl", action="store_true",
                   help="only the shared libraries of ?dl=1: DIST/www/dl/*.wasm to the bucket, then libs.json (see the top)")
    p.add_argument("--dl-base", default=os.environ.get("RV_DL_BASE", DL_BASE),
                   help="where the page will fetch the libraries from (%s)" % DL_BASE)
    a = p.parse_args()

    if a.boot_only:
        root = os.path.join(a.dist, "disk")
        for name in a.disk or sorted(d for d in os.listdir(root) if os.path.exists(os.path.join(root, d, "manifest.json"))):
            path = os.path.join(root, name, "manifest.json")
            with open(path) as f:
                m = json.load(f)
            if not m.get("hashes"):
                continue
            boot = boot_list(a.boot_dir, name, m, m["hashes"])
            warm = boot_list(a.boot_dir, name, m, m["hashes"], "warm", "-warm")
            if (boot is None or m.get("boot") == boot) and (warm is None or m.get("warm") == warm):
                print("r2-publish: %s/manifest.json: boot list %s" % (name, "already is" if boot else "none") +
                      (" (%d pieces)" % len(boot) if boot else "") + ("; warm list already is (%d pieces)" % len(warm) if warm else ""))
                continue
            new = dict(m)
            if boot is not None:
                new["boot"] = boot
            if warm is not None:
                new["warm"] = warm
            write_manifest(path, new)
            print("r2-publish: %s/manifest.json now has a boot list of %d pieces%s" % (
                name, len(boot or []), " and a warm list of %d" % len(warm) if warm else ""))
        return 0

    env = {k: os.environ.get(k, "") for k in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY")}
    if not all(env.values()) or not (a.base or a.dl):
        missing = [k for k, v in env.items() if not v] + ([] if a.base or a.dl else ["RV_PIECE_BASE"])
        print("r2-publish: no object storage configured (%s not set): the pieces stay on the site itself" %
              ", ".join(missing))
        return 0
    base = a.base if a.base.endswith("/") else a.base + "/"
    prefix = urllib.parse.urlsplit(base).path.lstrip("/")
    bucket = Bucket(env["R2_ENDPOINT"], env["R2_BUCKET"], env["R2_ACCESS_KEY_ID"], env["R2_SECRET_ACCESS_KEY"])
    if a.dl:
        return publish_dl(a, bucket)

    root = os.path.join(a.dist, "disk")
    names = a.disk or sorted(d for d in os.listdir(root) if os.path.exists(os.path.join(root, d, "manifest.json")))
    if not names:
        print("r2-publish: no disks in %s" % root, file=sys.stderr)
        return 1

    # 1. hash
    disks = {}       # name -> (manifest, hashes per piece)
    objects = {}     # key -> (path, size), unique
    pieces = 0
    t0 = time.time()
    for name in names:
        d = os.path.join(root, name)
        with open(os.path.join(d, "manifest.json")) as f:
            m = json.load(f)
        n = -(-m["size"] // m["piece"])
        present = [(int(m["present"][i >> 2], 16) >> (3 - (i & 3))) & 1 for i in range(n)]
        given = m.get("hashes")
        hashes = []

        def one(i):
            return sha256_file(os.path.join(d, "%06d" % i)) if present[i] else ""

        with concurrent.futures.ThreadPoolExecutor(4) as ex:
            hashes = list(ex.map(one, range(n)))
        if given and given != hashes:
            print("r2-publish: %s: the hashes in the manifest are not those of the pieces" % name, file=sys.stderr)
            return 1
        for i, h in enumerate(hashes):
            if h:
                pieces += 1
                path = os.path.join(d, "%06d" % i)
                objects.setdefault(prefix + h + EXTENSION, (path, os.path.getsize(path)))
        disks[name] = (m, hashes)
    total = sum(s for _, s in objects.values())
    print("r2-publish: %d pieces in %d disks, %d distinct (%.1f MiB), hashed in %.0fs" % (
        pieces, len(names), len(objects), total / 1048576, time.time() - t0))

    # 2. upload what is missing
    have = bucket.list(prefix)
    wrong = [k for k, (_, s) in objects.items() if k in have and have[k] != s]
    if wrong:
        print("r2-publish: %d objects exist with another size (e.g. %s): refusing" % (len(wrong), wrong[0]), file=sys.stderr)
        return 1
    missing = [k for k in objects if k not in have]
    print("r2-publish: %d of them are in the bucket already, %d to upload (%.1f MiB)" % (
        len(objects) - len(missing), len(missing), sum(objects[k][1] for k in missing) / 1048576))
    if a.dry_run:
        return 0
    done = [0]
    lock = threading.Lock()

    def up(key):
        path, size = objects[key]
        with open(path, "rb") as f:
            body = f.read()
        bucket.put(key, body)
        with lock:
            done[0] += 1
            if done[0] % 200 == 0 or done[0] == len(missing):
                print("r2-publish: uploaded %d / %d (%.0fs)" % (done[0], len(missing), time.time() - t0), flush=True)

    with concurrent.futures.ThreadPoolExecutor(max(1, a.jobs)) as ex:
        for f in [ex.submit(up, k) for k in missing]:
            f.result()

    # check: every object is there, with the size the piece has
    have = bucket.list(prefix)
    bad = [k for k, (_, s) in objects.items() if have.get(k) != s]
    if bad:
        print("r2-publish: %d objects are missing or the wrong size after the upload (e.g. %s): manifest not written" % (
            len(bad), bad[0]), file=sys.stderr)
        return 1
    print("r2-publish: bucket has all %d objects, sizes match (%d under %s)" % (len(objects), len(have), prefix))

    # 3. the manifests, last
    for name, (m, hashes) in disks.items():
        m = dict(m, base=base, hashes=hashes)
        boot = boot_list(a.boot_dir, name, m, hashes)
        if boot:
            m["boot"] = boot
        warm = boot_list(a.boot_dir, name, m, hashes, "warm", "-warm")
        if warm:
            m["warm"] = warm
        write_manifest(os.path.join(root, name, "manifest.json"), m)
        print("r2-publish: %s/manifest.json now says base %s%s" % (name, base, " and a boot list of %d pieces" % len(boot) if boot else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
