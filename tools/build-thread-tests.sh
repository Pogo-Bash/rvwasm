#!/bin/bash
# Build the thread tests (tests/threads) for riscv64. The C one three ways:
# static glibc, dynamic glibc and dynamic musl (sysroots as in
# tools/build-dyn-tests.sh, which has to have run first). The C++ one, which
# reaches threads through libstdc++, static and dynamic against glibc.
# Run inside the build VM.

set -e

S="$(realpath "$(dirname "$0")/..")"
W="${RVWASM_WORK:-$HOME/rvwasm}"
M="$W/sysroot-musl"
O="$W/build-threads"
GCC=riscv64-linux-gnu-gcc

mkdir -p "$O"
$GCC -O2 -Wall -static -pthread -o "$O/threads-static" "$S/tests/threads/threads.c"
$GCC -O2 -Wall -pthread -o "$O/threads-glibc" "$S/tests/threads/threads.c"

MUSL_CC=($GCC -O2 -Wall -nostdinc -isystem "$M/usr/include" -isystem "$($GCC -print-file-name=include)" -nostdlib)
"${MUSL_CC[@]}" -fPIE -pie -Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1 -o "$O/threads-musl" \
	"$M/usr/lib/Scrt1.o" "$M/usr/lib/crti.o" "$S/tests/threads/threads.c" \
	-L"$M/usr/lib" -lc "$($GCC -print-libgcc-file-name)" "$M/usr/lib/crtn.o"

# Memory that grows while a thread is parked (static, so that it also runs where there is no sysroot).
$GCC -O2 -Wall -static -pthread -o "$O/bigmem" "$S/tests/threads/bigmem.c"

# Do threads run at the same time? (Prints times: not one of the tests that are compared.)
$GCC -O2 -Wall -static -pthread -o "$O/par" "$S/tests/threads/par.c"

# Floating point, timed (static against glibc; the checksums are exact, the times are not for comparing).
$GCC -O2 -Wall -static -o "$O/fpbench" "$S/tests/threads/fpbench.c" -lm

# Floating point on random operands: every bit of every result.
$GCC -O2 -Wall -static -o "$O/fprand" "$S/tests/threads/fprand.c" -lm

# Waits with every signal blocked while the world is stopped, and the mask across exec.
$GCC -O2 -Wall -static -pthread -o "$O/sigmask" "$S/tests/threads/sigmask.c"

# Descriptors across fork and exec.
$GCC -O2 -Wall -static -o "$O/forkfd" "$S/tests/threads/forkfd.c"

GXX=riscv64-linux-gnu-g++
$GXX -O2 -Wall -std=c++17 -static -pthread -o "$O/threads-cxx-static" "$S/tests/threads/cxx.cc"
$GXX -O2 -Wall -std=c++17 -pthread -o "$O/threads-cxx" "$S/tests/threads/cxx.cc"
ls -la "$O"
