#!/bin/bash
# Brings a dist that was built before the relay's DNS allowlist up to date, without building anything: dev.html learns
# the relay's path and resolver from rv-site.json (net_path, dns), and the boot image gets the /init that writes
# nameserver 10.0.5.1 into /etc/resolv.conf (dns= on the kernel command line), by appending it to the initramfs
# (the later entry wins, as in tools/mk-initramfs.sh). A dist built from this tree already has both; running it
# again changes nothing.
#
#   docker/patch-dist.sh DIST        (DIST/www/dev.html and DIST/www/initramfs-wasm64_nommu.cpio.gz)
set -e
S="$(cd "$(dirname "$0")/.." && pwd)"
D=${1:?usage: patch-dist.sh DIST}
W=$D/www
[ -e "$W/dev.html" ] || { echo "patch-dist: no $W/dev.html" >&2; exit 1; }
cp "$S/tools/www/dev.html" "$W/dev.html"
img=$W/initramfs-wasm64_nommu.cpio.gz
if gzip -dc "$img" | cpio -i --to-stdout init 2> /dev/null | grep -q 'dns=10\.0\.5'; then
	echo "patch-dist: the boot image has the resolver already"
	exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gzip -dc "$img" > "$tmp/i.cpio"
mkdir "$tmp/root"
cp "$S/tools/initramfs-init" "$tmp/root/init"
chmod 755 "$tmp/root/init"
(cd "$tmp/root" && echo init | cpio -o --format=newc -A -O "$tmp/i.cpio" --owner=0:0 2> /dev/null)
gzip -c "$tmp/i.cpio" > "$img.tmp"
mv "$img.tmp" "$img"
echo "patch-dist: dev.html and the boot image's /init updated"
