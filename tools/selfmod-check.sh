#!/bin/bash
# Run tests/mm/selfmod.c (code that a program makes and changes while it
# runs) under qemu-riscv64 and under rv, and compare what they print. Run
# inside the build VM.
#
# Usage: selfmod-check.sh RV
#
# rv runs it three ways: as it is (natively that is the interpreter); with
# RV_JIT_FAKE=1, where the translator does everything but run what it makes
# and the program ends if a translation that is no longer the code would
# have run (rv/jit.c); and the same with fork done by a second emulator. The
# translator itself is tested in the browser: tests/selfmod.sh.

set -u

RV="$(realpath "$1")"
S="$(realpath "$(dirname "$0")/..")"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

riscv64-linux-gnu-gcc -O2 -Wall -static -pthread -o "$T/selfmod" "$S/tests/mm/selfmod.c" || exit 1
cd "$T" || exit 1
# (One case is not qemu-riscv64's to get right: the file says why, and what is right.)
expected="$(timeout 300 qemu-riscv64 ./selfmod 2>&1; echo "twice: 560000"; echo "[exit $?]")"

pass=0
fail=0
check() { # name, environment...
	local name="$1" actual
	shift
	actual="$(env "$@" timeout 300 "$RV" ./selfmod 2>&1; env "$@" timeout 300 "$RV" ./selfmod twice 2>&1; echo "[exit $?]")"
	if [ "$expected" == "$actual" ]; then
		pass=$((pass + 1))
		echo "ok    $name"
	else
		fail=$((fail + 1))
		echo "FAIL  $name"
		diff <(echo "$expected") <(echo "$actual") | sed 's/^/        /'
	fi
}

check "interpreter" RV_CHECK=selfmod
check "translator, not run" RV_JIT_FAKE=1
check "translator, not run, eager" RV_JIT_FAKE=1 RV_JIT_HOT=3 RV_JIT_HOT_MADE=2
check "translator, not run, fork by a second emulator" RV_JIT_FAKE=1 RV_FORK=snapshot
echo "selfmod: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
