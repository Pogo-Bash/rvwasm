#!/bin/bash
# Build the guest for the full-system comparison: a root filesystem image with
# the same static riscv64 BusyBox and CoreMark that rv runs, plus the
# benchmark script. The kernel is built separately (~/rvwasm/build-linux-rv,
# qemu-wasm's example config). Run inside the build VM.
#
# Output: ~/rvwasm/qemu-guest/{Image,rootfs.bin}
#
# With "bench" on the kernel command line the guest runs tests/bench.sh at
# boot; with "benchoff" it powers off afterwards.

set -e

S="$(realpath "$(dirname "$0")/..")"
W="$HOME/rvwasm"
OUT="$W/qemu-guest"
ROOT="$OUT/rootfs"

rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,bench,proc,sys,dev,tmp,etc/init.d,root}
cp "$W/busybox-rv/busybox" "$ROOT/bin/busybox"
for applet in $(qemu-riscv64 "$W/busybox-rv/busybox" --list); do
	[ "$applet" = busybox ] || ln -s busybox "$ROOT/bin/$applet"
done
cp "$W/guest-extra/coremark" "$ROOT/bench/coremark"
cp "$S/tests/bench.sh" "$S/tests/smoke.sh" "$ROOT/bench/"

cat > "$ROOT/etc/init.d/rcS" <<'EOF'
#!/bin/busybox sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /tmp
export PATH=/bin
if grep -q bench /proc/cmdline; then
	cd /tmp
	COREMARK=/bench/coremark BUSYBOX=/bin/busybox sh /bench/bench.sh
	grep -q benchoff /proc/cmdline && poweroff -f
fi
EOF
chmod 755 "$ROOT/etc/init.d/rcS"

rm -f "$OUT/rootfs.bin"
dd if=/dev/zero of="$OUT/rootfs.bin" bs=1M count=16 status=none
mke2fs -q -d "$ROOT" "$OUT/rootfs.bin"
cp "$W/build-linux-rv/arch/riscv/boot/Image" "$OUT/Image"
ls -la "$OUT/Image" "$OUT/rootfs.bin"
