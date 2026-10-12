#!/bin/bash
# Run the dynamic-linking tests (tools/build-dyn-tests.sh) under qemu-riscv64
# and under rv, each with the matching sysroot, and compare output and exit
# status. Run inside the build VM.
#
# Usage: dyn-check.sh RV

set -u

RV="$(realpath "$1")"
O="${RVWASM_WORK:-$HOME/rvwasm}/build-dyn"
MUSL="${RVWASM_WORK:-$HOME/rvwasm}/sysroot-musl"
GLIBC=/usr/riscv64-linux-gnu

pass=0
fail=0
check() { # sysroot, program, [argument]
	local root="$1"
	shift
	local expected actual
	expected="$(cd "$O" && qemu-riscv64 -L "$root" "$@" 2>&1; echo "[exit $?]")"
	actual="$(cd "$O" && timeout 30 "$RV" -L "$root" "$@" 2>&1; echo "[exit $?]")"
	if [ "$expected" == "$actual" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: $*"
		diff <(echo "$expected") <(echo "$actual") | head -8 | sed 's/^/    /'
	fi
}

check "$MUSL" ./dyn-musl
check "$MUSL" ./dyn-musl ./libgreet-musl.so
check "$MUSL" ./dyn-musl ./no-such-library.so
check "$GLIBC" ./dyn-glibc
check "$GLIBC" ./dyn-glibc ./libgreet-glibc.so
check "$GLIBC" ./dyn-glibc-nopie ./libgreet-glibc.so
[ -x "$O/cxx-glibc" ] && check "$GLIBC" ./cxx-glibc

echo "dynamic linking: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
