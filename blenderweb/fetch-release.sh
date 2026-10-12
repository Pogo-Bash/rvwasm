#!/bin/bash
# Puter's Blender for the browser (https://github.com/HeyPuter/blender-wasm), as they publish it built:
# fetched once, checked, unpacked and decompressed into DIR (default ~/rvwasm/blender-web, in the build VM),
# where blenderweb/serve.py serves it to the page.
#
#   blenderweb/fetch-release.sh [DIR] [TARBALL]     (TARBALL: a copy already on disk, else it is downloaded)
#
# What is kept of the release: the engine (blender.wasm, decompressed from blender.wasm.zst), its JavaScript
# (blender.js, emscripten's), Blender's scripts and data files with Python's library (assets.tar, from
# assets.tar.zst), and the shader translations they ship (wgsl-cache.json). Their index.html and its bundle
# are not kept: the page that starts Blender here is our own (blenderweb/www/), so nothing loads from
# anywhere but this computer (theirs has an analytics script and Google's fonts).
#
# The license is the GNU GPL, version 2 or later (Blender's), with Puter's build scripts under the same
# terms. SOURCE.txt, written next to the files, says where the source of exactly this build is.
set -e
DIR="${1:-$HOME/rvwasm/blender-web}"
TARBALL="$2"
URL=https://github.com/HeyPuter/blender-wasm/releases/download/1.0.0/blender7-1.tar.gz
SHA256=1a7a2387fecf92933f85a5521db79455c3b97cc2b8fc33b43fcbc09aff1fa6c3

mkdir -p "$DIR"
work="$DIR/.fetch"
rm -rf "$work"
mkdir -p "$work"
if [ -z "$TARBALL" ]; then
	TARBALL="$work/blender7-1.tar.gz"
	curl -fL --retry 3 -o "$TARBALL" "$URL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c -
tar --warning=no-unknown-keyword -xzf "$TARBALL" -C "$work" dist/blender.js dist/blender.wasm.zst dist/assets.tar.zst dist/manifest.json dist/wgsl-cache.json
zstd -q -d -f "$work/dist/blender.wasm.zst" -o "$DIR/blender.wasm"
zstd -q -d -f "$work/dist/assets.tar.zst" -o "$DIR/assets.tar"
cp "$work/dist/blender.js" "$work/dist/wgsl-cache.json" "$work/dist/manifest.json" "$DIR/"
# (The manifest says how big the decompressed files are: a check that they came out whole.)
python3 - "$DIR" <<'PY'
import json, os, sys
d = sys.argv[1]
m = json.load(open(os.path.join(d, "manifest.json")))
for packed, size in m.items():
    name = packed[:-4]
    have = os.path.getsize(os.path.join(d, name))
    if have != size:
        sys.exit("fetch-release: %s is %d bytes, the manifest says %d" % (name, have, size))
    print("fetch-release: %s %d bytes" % (name, have))
PY
cat > "$DIR/SOURCE.txt" <<TXT
Blender compiled to WebAssembly with a WebGPU backend, by Puter: release 1.0.0, blender7-1.tar.gz
($URL, sha256 $SHA256).
License: GNU General Public License, version 2 or later (Blender's license; Puter's build scripts are
under the GPL-2.0 too).
Source: the build harness, https://github.com/HeyPuter/blender-wasm (its Makefile and scripts/ say how
it is built), and Blender as patched for the web, https://github.com/HeyPuter/blender at
6b031d3d41c392883e3c495aa72343e10d15b43d (the Makefile's BLENDER_REF).
Unpacked here by blenderweb/fetch-release.sh: blender.wasm and assets.tar are the release's .zst files
decompressed, nothing else changed.
TXT
rm -rf "$work"
echo "fetch-release: done, in $DIR"
