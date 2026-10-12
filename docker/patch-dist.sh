#!/bin/bash
# Brings a dist up to date with this tree's page files and boot script without building anything. The build's dist is
# made from the build VM's copy of the tree, which can be a release behind; this is the one place that says what the
# live site has from *this* tree instead:
#   - www/dev.html, www/disk-worker.js and www/blenderweb.js are this tree's (tools/www/);
#   - the boot image gets this tree's /init (tools/initramfs-init) and desktop script (tools/x/desktop) appended to the initramfs (the later entry wins, as in
#     tools/mk-initramfs.sh), unless its last /init already is that file. (The init writes nameserver 10.0.5.1 into
#     /etc/resolv.conf: dns= on the kernel command line.)
# tools/deploy.sh runs this in the build VM on the dist before it copies it, and docker/release.sh's dist step runs it,
# so a plain deploy gives the live site exactly this. Running it again changes nothing.
#
#   docker/patch-dist.sh DIST        (DIST/www/dev.html, disk-worker.js, blenderweb.js and initramfs-wasm64_nommu.cpio.gz)
set -e
S="$(cd "$(dirname "$0")/.." && pwd)"
D=${1:?usage: patch-dist.sh DIST}
W=$D/www
[ -e "$W/dev.html" ] || { echo "patch-dist: no $W/dev.html" >&2; exit 1; }
for f in dev.html disk-worker.js blenderweb.js gpu-exec.js; do
	cmp -s "$S/tools/www/$f" "$W/$f" || { cp "$S/tools/www/$f" "$W/$f"; echo "patch-dist: $f updated"; }
done
img=$W/initramfs-wasm64_nommu.cpio.gz
# Files of this tree the boot image must carry, as "path in the image:path in the tree". The desktop script: the
# machine runs the boot image's copy, not the disk's (tools/debian-enter binds /usr/lib/wlserver/desktop over it).
files="init:tools/initramfs-init usr/lib/wlserver/desktop:tools/x/desktop"
stale=
for e in $files; do
	dst=${e%%:*} src=$S/${e#*:}
	size=$(wc -c < "$src")
	# (--to-stdout prints every entry of that name, the first ones first; the last is the one that counts)
	gzip -dc "$img" | cpio -i --to-stdout "$dst" 2> /dev/null | tail -c "$size" | cmp -s - "$src" || stale="$stale $e"
done
if [ -z "$stale" ]; then
	echo "patch-dist: the boot image has this tree's files already"
	exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gzip -dc "$img" > "$tmp/i.cpio"
mkdir "$tmp/root"
for e in $stale; do
	dst=${e%%:*} src=$S/${e#*:}
	mkdir -p "$tmp/root/$(dirname "$dst")"
	cp "$src" "$tmp/root/$dst"
	chmod 755 "$tmp/root/$dst"
	touch -d "@${SOURCE_DATE_EPOCH:-0}" "$tmp/root/$dst"  # (with --reproducible below: the same inputs give the same image)
	(cd "$tmp/root" && echo "$dst" | cpio -o --format=newc -A -O "$tmp/i.cpio" --owner=0:0 --reproducible 2> /dev/null)
	echo "patch-dist: the boot image's /$dst updated"
done
gzip -c "$tmp/i.cpio" > "$img.tmp"
mv "$img.tmp" "$img"
