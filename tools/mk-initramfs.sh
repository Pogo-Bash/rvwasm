#!/bin/bash
# Build an initramfs for the dev server: upstream's (BusyBox + init) plus our files.
#
# Usage: mk-initramfs.sh STAGING_DIR OUTPUT.cpio.gz
#
# STAGING_DIR is laid out like the root filesystem (e.g. STAGING_DIR/bin/rv).

set -e

: "${LW_ROOT:=${RVWASM_WORK:-$HOME/rvwasm}/linux-wasm}"
: "${LW_VARIANT:=wasm64_nommu}"

staging="$(realpath "$1")"
output="$(realpath -m "$2")"
base="$LW_ROOT/workspace/install/initramfs-$LW_VARIANT/initramfs.cpio.gz"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

gzip -dc "$base" > "$tmp/initramfs.cpio"
(
	cd "$staging"
	# Must run in the directory being archived (paths are recorded relative to it).
	find . -mindepth 1 -print0 | cpio --null -o --format=newc -A -O "$tmp/initramfs.cpio" --owner=0:0 2>/dev/null
)
gzip -c "$tmp/initramfs.cpio" > "$output.tmp"
mv "$output.tmp" "$output"
ls -la "$output"
