#!/bin/bash
# Build the dynamic-linking tests (tests/dyn) for riscv64, against musl and
# against glibc, and lay out a sysroot for each. Run inside the build VM.
#
#   ~/rvwasm/sysroot-musl    riscv64 musl, built here from the 1.2.5 release
#   /usr/riscv64-linux-gnu   riscv64 glibc, from the distribution's cross packages
#
# Run a test with:  rv -L SYSROOT ~/rvwasm/build-dyn/NAME [LIBRARY]

set -e

S="$(realpath "$(dirname "$0")/..")"
W="${RVWASM_WORK:-$HOME/rvwasm}"
M="$W/sysroot-musl"
O="$W/build-dyn"
GCC=riscv64-linux-gnu-gcc

if [ ! -f "$M/usr/lib/libc.so" ]; then
	# The wasm patch is the single commit on top of the release.
	[ -d "$W/musl-rv-src" ] || git -C "$W/linux-wasm/workspace/src/musl" worktree add -q --detach "$W/musl-rv-src" HEAD~1
	mkdir -p "$W/musl-rv-src/build"
	(
		cd "$W/musl-rv-src/build"
		../configure --target=riscv64-linux-gnu --prefix=/usr --syslibdir=/lib \
			CROSS_COMPILE=riscv64-linux-gnu- CC=$GCC > configure.log 2>&1
		make -j"$(nproc)" > make.log 2>&1
		make DESTDIR="$M" install > install.log 2>&1
	)
fi
# musl installs its loader as an absolute link; inside a sysroot it has to be relative.
ln -sfn ../usr/lib/libc.so "$M/lib/ld-musl-riscv64.so.1"

mkdir -p "$O"

# musl: the distribution's compiler targets glibc, so name everything explicitly.
MUSL_CC=($GCC -O2 -nostdinc -isystem "$M/usr/include" -isystem "$($GCC -print-file-name=include)" -nostdlib)
MUSL_LIBS=(-L"$M/usr/lib" -lc "$($GCC -print-libgcc-file-name)")
"${MUSL_CC[@]}" -fPIC -shared -o "$O/libgreet-musl.so" \
	"$M/usr/lib/crti.o" "$S/tests/dyn/libgreet.c" "${MUSL_LIBS[@]}" "$M/usr/lib/crtn.o"
"${MUSL_CC[@]}" -fPIE -pie -Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1 -o "$O/dyn-musl" \
	"$M/usr/lib/Scrt1.o" "$M/usr/lib/crti.o" "$S/tests/dyn/main.c" "${MUSL_LIBS[@]}" "$M/usr/lib/crtn.o"

# glibc: the compiler's defaults (dynamic, position independent).
$GCC -O2 -fPIC -shared -o "$O/libgreet-glibc.so" "$S/tests/dyn/libgreet.c"
$GCC -O2 -o "$O/dyn-glibc" "$S/tests/dyn/main.c" -lm -ldl
# And a fixed-address dynamic executable, the other kind a loader meets.
$GCC -O2 -no-pie -o "$O/dyn-glibc-nopie" "$S/tests/dyn/main.c" -lm -ldl

# C++, if the cross compiler is installed (apt install g++-riscv64-linux-gnu).
if command -v riscv64-linux-gnu-g++ > /dev/null; then
	riscv64-linux-gnu-g++ -O2 -o "$O/cxx-glibc" "$S/tests/dyn/cxx.cc"
fi

ls -la "$O"
