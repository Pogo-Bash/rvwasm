#!/bin/bash
# chroot() under rv (tests/chroot/chroot.c), compared with qemu-riscv64, which
# passes the call to the kernel: as it is, with forks as snapshots, and with a
# sysroot (-L) that the new root is inside of. Runs as root (sudo): chroot()
# takes the right to. Run inside the build VM.
#
# RV must be linked statically for the exec inside the new root: a native rv
# that needs the host's dynamic linker cannot start where there is none.
# (On Linux/Wasm rv is a program of its own, with nothing to look for.)
#   make -C rv O=DIR LDFLAGS=-static
#
# qemu answers readlink("/proc/self/exe") itself, also where the root has no
# /proc: that one line is the kernel's answer (ENOENT) under rv and is left
# out of the comparison.
#
# Usage: chroot-check.sh RV [TEST-BINARY]

set -u

RV="$(realpath "$1")"
T="${2:-${RVWASM_WORK:-$HOME/rvwasm}/guest-extra/chroot}"
W="$(mktemp -d)"
trap 'sudo rm -rf "$W"' EXIT

pass=0
fail=0
sudo env -i qemu-riscv64 "$T" "$W/q" 2>&1 | grep -v '/proc/self/exe' | sed 's/^chroot: FAILED$/chroot: all ok/' > "$W/q.out"

check() { # name, then how rv is run
	local name="$1"
	shift
	if diff <(grep -v '/proc/self/exe' "$W/$name.out") "$W/q.out" > "$W/$name.diff" &&
	   grep -q '^no /proc/self/exe in the new root: ok$' "$W/$name.out" &&
	   [ "$(tail -1 "$W/$name.out")" == "chroot: all ok" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: $name"
		head -20 "$W/$name.diff" | sed 's/^/    /'
		tail -4 "$W/$name.out" | sed 's/^/    /'
	fi
}

sudo env -i timeout 120 "$RV" "$T" "$W/plain" > "$W/plain.out" 2>&1
check plain
sudo env -i RV_FORK=snapshot timeout 120 "$RV" "$T" "$W/snapshot" > "$W/snapshot.out" 2>&1
check snapshot
# With a sysroot: the program and the new root are under it, and the guest names them from it. (Not
# right under it: a path whose directory is the sysroot itself is the host's, syscall.c: user_path().)
mkdir -p "$W/sysroot/tmp"
cp "$T" "$W/sysroot/chroot"
sudo env -i timeout 120 "$RV" -L "$W/sysroot" "$W/sysroot/chroot" /tmp/jail > "$W/sysroot.out" 2>&1
[ -d "$W/sysroot/tmp/jail" ] || echo "    (the new root was not made under the sysroot)" >> "$W/sysroot.out"
check sysroot

echo "chroot: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
