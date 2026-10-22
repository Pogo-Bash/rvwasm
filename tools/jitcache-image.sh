#!/bin/bash
# Make the read-only store of translated code that goes with a disk image (rv/jitcache.c, tools/www/jitcache.js):
# run some work on the machine under Node with an empty store, and write what it translated as jitcache.bin with
# jitcache.json ({"sha256", "size"}) next to it. The page starts its store from these when it has kept none of its
# own (after checking the SHA-256), so programs that ran here start warm on a new browser too. Run on the Mac:
#
#   tools/with-browser.sh WHO tools/jitcache-image.sh NMDIR DISK OUTDIR ['COMMAND']
#
# NMDIR: rt/, vmlinux.wasm, initramfs.cpio.gz and rv.sha256 of the build that will be served (as a worktree's
# build/nm); DISK: the disk the dev server has (daily; none for none); OUTDIR: where jitcache.bin and jitcache.json go (copy them
# into the dev server's disk/DISK/ afterwards). COMMAND: the work (default: starting python3, gcc, g++ and some
# small programs, as tests/jitcache-warm.sh does). A heavy machine: run it under with-browser.sh.
#
# The records are only good for the same rv (its translator's sources are in every key) and the same kind of host
# (whether it fuses multiply-adds, which Node's engine and Chrome's may answer alike or not): a record that does
# not fit is not found, nothing worse.

set -e
N=$1 DISK=$2 OUT=$3
CMD=${4:-'cp /rv/jitcache-warm.sh /debian/root/; debian "bash /root/jitcache-warm.sh image"'}
[ -n "$N" ] && [ -n "$DISK" ] && [ -n "$OUT" ] || { echo "usage: $0 NMDIR DISK OUTDIR [COMMAND]" >&2; exit 2; }
cd "$(dirname "$0")/.."
mkdir -p "$OUT"
rm -f "$OUT/jitcache.bin" "$OUT/jitcache.json"
node --experimental-wasm-memory64 tools/node-machine.js --runtime "$N/rt" --kernel "$N/vmlinux.wasm" \
	--initramfs "$N/initramfs.cpio.gz" $([ "$DISK" = none ] || echo --disk "$DISK") --timeout 3600000 \
	--jitcache "$OUT/jitcache.bin:384" --jitcache-program "$(cat "$N/rv.sha256")" "$CMD"
size=$(wc -c < "$OUT/jitcache.bin" | tr -d ' ')
sum=$(shasum -a 256 "$OUT/jitcache.bin" | cut -c1-64)
printf '{"sha256": "%s", "size": %s}\n' "$sum" "$size" > "$OUT/jitcache.json"
cat "$OUT/jitcache.json"
