#!/bin/bash
# Run the system call filter tests (tests/seccomp, built static into
# ~/rvwasm/guest-extra by tools/stage.sh) under rv: as they are, with the
# host's memfd_create() withheld (RV_MEMFD=0: the filters then cross an exec
# through a pipe, as they do on Linux/Wasm), and with forks as snapshots.
# Run inside the build VM.
#
# Usage: seccomp-check.sh RV [DIR]

set -u

RV="$(realpath "$1")"
D="${2:-${RVWASM_WORK:-$HOME/rvwasm}/guest-extra}"

pass=0
fail=0
check() { # program, its last line when all is well, environment...
	local name="$1" expected="$2" actual
	shift 2
	actual="$(/usr/bin/env -i "$@" timeout 120 "$RV" "$D/$name" 2>&1)"
	if [ "$(tail -1 <<< "$actual")" == "$expected" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: $name $*"
		tail -6 <<< "$actual" | sed 's/^/    /'
	fi
}

check seccomp "seccomp: all ok"
check seccomp "seccomp: all ok" RV_MEMFD=0
check seccomp "seccomp: all ok" RV_FORK=snapshot
check seccomp "seccomp: all ok" RV_FORK=snapshot RV_MEMFD=0
check seccomp-strict "strict: write and exit ok, another call ok"
check seccomp-strict "strict: write and exit ok, another call ok" RV_FORK=snapshot

echo "seccomp: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
