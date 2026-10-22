#!/bin/bash
# The translator under load: real programs with every block translated the
# first time it runs (RV_JIT_HOT=1 RV_JIT_START=0), natively, with
# RV_JIT_FAKE=1 (jit.c: regions are made, kept, suspended and checked as on
# Linux/Wasm, and the interpreter runs where they would). That makes tens of
# thousands of regions per process and fills, tidies and grows the map
# several times. Best run with an rv built with -fsanitize=address (and
# ASAN_OPTIONS=detect_stack_use_after_return=0, without which it crawls) or
# with -DRV_HEAPCHECK. Run inside the build VM.
#
# The last run has every third translation find no room (RV_JIT_REFUSE=3,
# as when other threads have used up the host's budget) while the map grows:
# the count of a block refused was once written into the table of counts the
# growth had freed (an abort in free() later, in the browser).
#
# Usage: jit-stress.sh RV [ROOT]     (ROOT: a Debian riscv64 root with g++, gcc and python3)
# RV_JIT_CACHE=FILE: with a store of translations (rv/jitcache.c), shared by all the programs run; twice, the
# second time finds most regions there.

set -u

RV="$(realpath "$1")"
R="$(realpath "${2:-${RVWASM_WORK:-$HOME/rvwasm}/debian-root-kde}")"
D="$R/tmp/jit-stress-$$"
mkdir -p "$D"
trap 'rm -rf "$D"' EXIT
printf '#include <iostream>\n#include <map>\n#include <string>\nint main() { std::map<std::string, int> m{{"c++", 20}}; std::cout << "c++ " << m.size() << "\\n"; }\n' > "$D/jc.cc"
printf '#include <stdio.h>\nint main(void) { printf("c\\n"); return 0; }\n' > "$D/jc.c"

pass=0
fail=0
EXTRA=()
check() { # expected output, command...
	local want="$1" got
	shift
	got="$(cd "$D" && /usr/bin/env -i "PATH=$R/usr/bin" HOME=/nonexistent LC_ALL=C TZ=UTC "RV_SYSROOT=$R" \
		ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:detect_stack_use_after_return=0}" \
		RV_JIT_FAKE=1 RV_JIT_HOT=1 RV_JIT_START=0 RV_VERBOSE=1 ${RV_FORK:+RV_FORK=$RV_FORK} ${RV_JIT_CACHE:+RV_JIT_CACHE=$RV_JIT_CACHE} "${EXTRA[@]}" \
		/usr/bin/timeout 1800 "$RV" "$@" 2> "$D/err"; echo "[exit $?]")"
	if [ "$got" == "$want" ] && ! grep -q -E 'ERROR: AddressSanitizer|runtime error|heapcheck \(|stale translation' "$D/err"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL: $*"
		echo "$got" | tail -n 3 | sed 's/^/    /'
		grep -v '^rv: ' "$D/err" | head -n 20 | sed 's/^/    /'
	fi
	grep -E '^rv: jit: [0-9]+ regions' "$D/err" | sort -t' ' -k3 -n -r | head -n 1 | sed 's/^/    largest: /'
}

check "[exit 0]" "$R/usr/bin/g++" -O2 -std=c++20 jc.cc -o jc-cxx
check "c++ 1
[exit 0]" ./jc-cxx
check "[exit 0]" "$R/usr/bin/gcc" -O2 jc.c -o jc-c
check "44999850000
[exit 0]" "$R/usr/bin/python3" -c 'import json, re; print(sum(range(300000)))'
EXTRA=(RV_JIT_REFUSE=3)
check "44999850000
[exit 0]" "$R/usr/bin/python3" -c 'import json, re; print(sum(range(300000)))'
EXTRA=()

# The thread tests (tools/thread-check.sh) with every block translated: threads that fork while
# another thread has the translator's buffers (the child once waited for them for good).
if "$(dirname "$(realpath "$0")")/thread-check.sh" "$RV" RV_JIT_FAKE=1 RV_JIT_HOT=1 RV_JIT_START=0 \
	${RV_FORK:+RV_FORK=$RV_FORK} ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:detect_stack_use_after_return=0}" > "$D/threads" 2>&1; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
	echo "FAIL: thread-check.sh with RV_JIT_FAKE=1"
	sed 's/^/    /' "$D/threads" | head -n 30
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
