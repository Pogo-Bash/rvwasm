#!/bin/bash
# Run the thread tests (tools/build-thread-tests.sh) under qemu-riscv64 and
# under rv and compare output and exit status. Run inside the build VM.
#
# Usage: thread-check.sh RV [NAME=VALUE...]     (extra environment for rv, e.g. RV_FORK=snapshot)

set -u

RV="$(realpath "$1")"
shift
O="${RVWASM_WORK:-$HOME/rvwasm}/build-threads"
MUSL="${RVWASM_WORK:-$HOME/rvwasm}/sysroot-musl"
GLIBC=/usr/riscv64-linux-gnu

pass=0
fail=0
check() { # sysroot, program
	local expected actual
	expected="$(qemu-riscv64 -L "$1" "$O/$2" 2>&1; echo "[exit $?]")"
	# (SIGKILL, to timeout's whole process group: a forked child that hangs with SIGTERM blocked goes too,
	# and does not keep the output open for good.)
	actual="$(env "${EXTRA[@]}" timeout -s KILL 120 "$RV" -L "$1" "$O/$2" 2>&1; echo "[exit $?]")"
	if [ "$expected" == "$actual" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: $2"
		diff <(echo "$expected") <(echo "$actual") | sed 's/^/    /'
	fi
}

EXTRA=(RV_CHECK=threads "$@")	# (never empty, for env)
check "$GLIBC" threads-static
check "$GLIBC" threads-glibc
check "$MUSL" threads-musl
check "$GLIBC" threads-cxx-static
check "$GLIBC" threads-cxx
check "$GLIBC" bigmem
check "$GLIBC" forkfd
check "$GLIBC" sigmask
echo "threads: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
