#!/bin/bash
# Build the dynamic-linking prototype (in the build VM): ld-wasm (the C library and the dynamic linker),
# libfoo.so (a shared library: C and C++), and the test programs (position independent executables
# without a C library of their own). Output in out/.
#   tests/wasmdyn/build.sh
set -e
cd "$(dirname "$0")"
. ./env.sh
L=$NQ_SYSROOT/usr/lib
LD=$NQ_LLVM/wasm-ld
O=out
mkdir -p $O
# What every module ld-wasm loads is linked with: a 64-bit, shared-memory, position independent module.
LDFLAGS=(-mwasm64 --experimental-pic --shared-memory --max-memory=17179869184 --no-entry)

# --- ld-wasm: musl, libc++, libc++abi, the C++ exception and longjmp tags, the dynamic linker; every
# public symbol of those exported for the modules it loads (what a libc.so would export).
$CC "${CFLAGS[@]}" -c ldso/ldso.c -o $O/ldso.o
$CC "${CFLAGS[@]}" -c ldso/ldso_asm.S -o $O/ldso_asm.o
$CC "${CFLAGS[@]}" -c ldso/dlcrt.S -o $O/dlcrt.o
$NQ_LLVM/llvm-ar x --output $O $L/libnqcrt.a cpp_exception.o sjlj_tag.o nqsjlj.o comparetf2.o
arch=($L/libc.a $L/libc++.a $L/libc++abi.a $L/libunwind.a)
{
	$NQ_LLVM/llvm-nm --defined-only --extern-only --format=just-symbols "${arch[@]}" \
		$O/cpp_exception.o $O/sjlj_tag.o $O/nqsjlj.o $O/comparetf2.o $O/ldso.o 2>/dev/null
	echo __cpp_exception; echo __c_longjmp
} | grep -v -E ':$|^$|^(__wasm_call_ctors|__wasm_apply_data_relocs|__wasm_init_tls|main|__main_argc_argv|__main_void)$' \
	| sort -u | sed 's/^/--export-if-defined=/' > $O/ld-wasm.exports
echo "ld-wasm exports (asked): $(wc -l < $O/ld-wasm.exports)"
$CC "${CFLAGS[@]}" -nostdlib -o $O/ld-wasm $L/crt1.o $L/nqstart.o $O/ldso.o $O/ldso_asm.o \
	$O/cpp_exception.o $O/sjlj_tag.o $O/nqsjlj.o $O/comparetf2.o \
	-Wl,--whole-archive "${arch[@]}" -Wl,--no-whole-archive \
	$NQ_LLVM/../lib/clang/18/lib/libclang_rt.builtins-wasm64.a \
	-Wl,-mwasm64 -Wl,--experimental-pic -Wl,-pie -Wl,--no-merge-data-segments -Wl,--no-entry \
	-Wl,--export-if-defined=_start -Wl,--export-if-defined=__set_tls_base -Wl,--export-if-defined=__libc_clone_callback \
	-Wl,--export-if-defined=__wasm_apply_data_relocs -Wl,--export-if-defined=__libc_handle_signal \
	-Wl,--import-memory -Wl,--shared-memory -Wl,--max-memory=17179869184 -Wl,--import-table \
	-Wl,--error-limit=0 -Wl,@$O/ld-wasm.exports 2> >(grep -v -E 'creating shared libraries|signature mismatch' >&2)
python3 ~/wt/nativeqt/nativeqt/wasm-imports.py $O/ld-wasm | grep -v -E '^env\.(memory|__indirect_function_table|__stack_pointer|__memory_base|__table_base|__table_base32|__wasm_abort|__wasm_syscall_[0-6]|__wasm_dl) ' && { echo "ld-wasm: unexpected imports"; exit 1; } || true

# --- libfoo.so: a shared library. -fvisibility=default (clang's default for wasm is hidden); libunwind's
# Wasm part (one small object, hidden) in every module that has landing pads: its thread-local
# __wasm_lpad_context cannot be shared between modules with this LLVM (local-exec TLS only).
$CC "${CFLAGS[@]}" -fvisibility=default -c libfoo.c -o $O/libfoo.o
$CC "${CXXFLAGS[@]}" -fvisibility=default -c libfoo_cpp.cpp -o $O/libfoo_cpp.o
$LD "${LDFLAGS[@]}" -shared --export-if-defined=__wasm_call_ctors $O/libfoo.o $O/libfoo_cpp.o $O/dlcrt.o $L/libunwind.a -o $O/libfoo.so

# libbig.so: 20000 exported functions (about what libQt6Gui or libQt6Qml export), for main5.
python3 -c '
n = 20000
print("int big_0(int x) { return x; }")
for i in range(1, n):
    print("int big_%d(int x) { return big_%d(x) + 1; }" % (i, i - 1) if i % 1000 == 0 else "int big_%d(int x) { return x + %d; }" % (i, i))
print("int big_last(int x) { return big_%d(x); }" % (n - 1))
' > $O/big.c
$CC "${CFLAGS[@]}" -fvisibility=default -O1 -c $O/big.c -o $O/big.o
$LD "${LDFLAGS[@]}" -shared --export-if-defined=__wasm_call_ctors $O/big.o $O/dlcrt.o -o $O/libbig.so
# The same functions, none exported but big_last, every one in the table (an array of their addresses):
# what instantiation costs without 20000 exports.
{ sed 's/^int big_/static int big_/; s/^static int big_last/int big_last/' $O/big.c
  echo "void *big_table[] = {"; python3 -c 'print(",".join("(void *)big_%d" % i for i in range(20000)))'; echo "};"
} > $O/big-noexp.c
$CC "${CFLAGS[@]}" -fvisibility=default -O1 -c $O/big-noexp.c -o $O/big-noexp.o
$LD "${LDFLAGS[@]}" -shared --export-if-defined=__wasm_call_ctors $O/big-noexp.o $O/dlcrt.o -o $O/libbig-noexp.so
# libbigcall.so: calls each of libbig.so's 20000 functions (imports them all), for main5.
python3 -c '
n = 20000
for i in range(n):
    print("int big_%d(int);" % i)
print("int bigcall_all(int x) {")
for i in range(n):
    print("  x = big_%d(x) & 7;" % i)
print("  return x;\n}")
' > $O/bigcall.c
$CC "${CFLAGS[@]}" -fvisibility=default -O1 -c $O/bigcall.c -o $O/bigcall.o
(cd $O && $LD "${LDFLAGS[@]}" -shared --export-if-defined=__wasm_call_ctors bigcall.o dlcrt.o libbig.so -o libbigcall.so)

# --- Programs: position independent executables, linked against libfoo.so (NEEDED) or not; their C
# library is ld-wasm's.
PFLAGS=("${LDFLAGS[@]}" -pie --import-memory --import-table --export-if-defined=__main_argc_argv
	--export-if-defined=__main_void --export=__wasm_call_ctors --export=__wasm_apply_data_relocs)
$CC "${CXXFLAGS[@]}" -c main1.cpp -o $O/main1.o
$CC "${CFLAGS[@]}" -c main2.c -o $O/main2.o
$CC "${CFLAGS[@]}" -c main3.c -o $O/main3.o
$CC "${CFLAGS[@]}" -c bench.c -o $O/bench.o
$CC "${CFLAGS[@]}" -c main4.c -o $O/main4.o
$CC "${CFLAGS[@]}" -c main5.c -o $O/main5.o
(cd $O && $LD "${PFLAGS[@]}" main1.o dlcrt.o $L/libunwind.a libfoo.so -o main1 2> >(grep -v 'creating shared' >&2))
$LD "${PFLAGS[@]}" $O/main2.o $O/dlcrt.o -o $O/main2 2> >(grep -v 'creating shared' >&2)
$LD "${PFLAGS[@]}" $O/main3.o $O/dlcrt.o -o $O/main3 2> >(grep -v 'creating shared' >&2)
$LD "${PFLAGS[@]}" $O/main5.o $O/dlcrt.o -o $O/main5 2> >(grep -v 'creating shared' >&2)
$LD "${PFLAGS[@]}" $O/main4.o $O/dlcrt.o -o $O/main4 2> >(grep -v 'creating shared' >&2)
(cd $O && $LD "${PFLAGS[@]}" bench.o dlcrt.o libfoo.so -o bench 2> >(grep -v 'creating shared' >&2))
# The same benchmark as one static program (libfoo.c linked in), as nq-clang links programs today.
NQ=~/wt/nativeqt/nativeqt/bin/nq-clang
$NQ -O2 -o $O/bench-static bench.c libfoo.c
# Start-up: the same hello as a static program and as one loaded by ld-wasm; timeit runs one N times.
$CC "${CFLAGS[@]}" -c hello.c -o $O/hello.o
$LD "${PFLAGS[@]}" $O/hello.o $O/dlcrt.o -o $O/hello 2> >(grep -v 'creating shared' >&2)
$NQ -O2 -o $O/hello-static hello.c
$NQ -O2 -o $O/timeit timeit.c
# Every exported function in a table slot by the module's own element segment (tablify.py); libbig-js.so
# without, for comparison (ld-wasm then has the host put them there from JavaScript, in every thread).
cp $O/libbig.so $O/libbig-js.so
for m in ld-wasm libfoo.so libbig.so libbigcall.so main1 main2 main3 main4 main5 bench hello; do
	python3 tablify.py ${DYNSYM---dynsym} $O/$m > /dev/null
done
ls -la $O/ld-wasm $O/libfoo.so $O/main1 $O/main2 $O/main3 $O/main4 $O/main5 $O/libbig.so $O/libbig-js.so $O/libbig-noexp.so $O/libbigcall.so $O/bench $O/bench-static $O/hello $O/hello-static $O/timeit
