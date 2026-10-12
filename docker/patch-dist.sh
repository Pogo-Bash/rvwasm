#!/bin/bash
# Brings a dist up to date with this tree's page files and boot script without building anything. The build's dist is
# made from the build VM's copy of the tree, which can be a release behind; this is the one place that says what the
# live site has from *this* tree instead:
#   - www/dev.html and www/disk-worker.js are this tree's (tools/www/);
#   - the boot image gets this tree's /init (tools/initramfs-init) appended to the initramfs (the later entry wins, as in
#     tools/mk-initramfs.sh), unless its last /init already is that file. (The init writes nameserver 10.0.5.1 into
#     /etc/resolv.conf: dns= on the kernel command line.)
# tools/deploy.sh runs this in the build VM on the dist before it copies it, and docker/release.sh's dist step runs it,
# so a plain deploy gives the live site exactly this. Running it again changes nothing.
#
#   docker/patch-dist.sh DIST        (DIST/www/dev.html, disk-worker.js and initramfs-wasm64_nommu.cpio.gz)
set -e
S="$(cd "$(dirname "$0")/.." && pwd)"
D=${1:?usage: patch-dist.sh DIST}
W=$D/www
[ -e "$W/dev.html" ] || { echo "patch-dist: no $W/dev.html" >&2; exit 1; }
for f in dev.html disk-worker.js; do
	cmp -s "$S/tools/www/$f" "$W/$f" || { cp "$S/tools/www/$f" "$W/$f"; echo "patch-dist: $f updated"; }
done
img=$W/initramfs-wasm64_nommu.cpio.gz
init=$S/tools/initramfs-init
size=$(wc -c < "$init")
# (--to-stdout prints every entry named init, upstream's first; the last is the one the kernel runs)
if gzip -dc "$img" | cpio -i --to-stdout init 2> /dev/null | tail -c "$size" | cmp -s - "$init"; then
	echo "patch-dist: the boot image has this /init already"
	exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gzip -dc "$img" > "$tmp/i.cpio"
mkdir "$tmp/root"
cp "$init" "$tmp/root/init"
chmod 755 "$tmp/root/init"
touch -d "@${SOURCE_DATE_EPOCH:-0}" "$tmp/root/init"  # (with --reproducible below: the same inputs give the same image)
(cd "$tmp/root" && echo init | cpio -o --format=newc -A -O "$tmp/i.cpio" --owner=0:0 --reproducible 2> /dev/null)
gzip -c "$tmp/i.cpio" > "$img.tmp"
mv "$img.tmp" "$img"
echo "patch-dist: the boot image's /init updated"
