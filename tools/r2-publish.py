#!/usr/bin/env python3
"""Put a dist's disk pieces on S3-style object storage (Cloudflare R2), content-addressed, and say so in the manifest.

    r2-publish.py DIST [--base URL] [--jobs N] [--dry-run] [--disk NAME ...]

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
     and nothing else changed (the `id` stays: browsers keep what they have of the disk).

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

    def put(self, key, body):
        status, data = self.call("PUT", key, body=body, headers={"content-type": CONTENT_TYPE,
                                                                  "cache-control": CACHE_CONTROL})
        if status >= 300:
            raise RuntimeError("PUT %s: %d %s" % (key, status, data[:200]))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dist")
    p.add_argument("--base", default=os.environ.get("RV_PIECE_BASE", ""),
                   help="where the page will fetch pieces from, e.g. https://disk.example.com/disk/")
    p.add_argument("--jobs", type=int, default=8, help="uploads at once (8)")
    p.add_argument("--dry-run", action="store_true", help="hash and compare, upload nothing, change nothing")
    p.add_argument("--disk", action="append", help="only this disk (default: all in DIST/disk)")
    a = p.parse_args()

    env = {k: os.environ.get(k, "") for k in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY")}
    if not all(env.values()) or not a.base:
        missing = [k for k, v in env.items() if not v] + ([] if a.base else ["RV_PIECE_BASE"])
        print("r2-publish: no object storage configured (%s not set): the pieces stay on the site itself" %
              ", ".join(missing))
        return 0
    base = a.base if a.base.endswith("/") else a.base + "/"
    prefix = urllib.parse.urlsplit(base).path.lstrip("/")
    bucket = Bucket(env["R2_ENDPOINT"], env["R2_BUCKET"], env["R2_ACCESS_KEY_ID"], env["R2_SECRET_ACCESS_KEY"])

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
        path = os.path.join(root, name, "manifest.json")
        tmp = path + ".part"
        with open(tmp, "w") as f:
            json.dump(m, f)
            f.write("\n")
        os.replace(tmp, path)
        print("r2-publish: %s/manifest.json now says base %s" % (name, base))
    return 0


if __name__ == "__main__":
    sys.exit(main())
