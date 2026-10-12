#!/bin/bash
# libc++, libc++abi, libunwind (LLVM 18, the toolchain's own sources) for wasm64 linux-wasm:
# static, threads, Wasm exception handling (-fwasm-exceptions; libunwind is then just Unwind-wasm.c).
set -e
. "$(dirname "$0")/env.sh"
B=$NQ_BUILD/runtimes
mkdir -p $B
# nqstart.o: crt/nqstart.c, needed by every link, every program after crt1.o (constructors after libc's setup).
# (Against musl's internal headers, for struct pthread; crt/nqstack.S moves the stack pointer.)
M=$LW/src/musl MB=$LW/build/musl-wasm64_nommu
nq-clang -c -O2 -Wno-int-to-void-pointer-cast -I$M/arch/wasm -I$M/arch/generic -I$MB/obj/src/internal \
	-I$M/src/include -I$M/src/internal -I$MB/obj/include -I$M/include -o $B/nqstart-c.o $NQ_TREE/crt/nqstart.c
nq-clang -c -o $B/nqstack.o $NQ_TREE/crt/nqstack.S
$NQ_LLVM/wasm-ld -r -mwasm64 -o $NQ_SYSROOT/usr/lib/nqstart.o $B/nqstart-c.o $B/nqstack.o
# Our copy of the runtimes' sources with patches/nativeqt-llvm18-runtimes.diff applied.
L=$NQ_SRC/llvm
if [ ! -d $L ]; then
	mkdir -p $L/llvm
	(cd $LW/src/llvm && cp -a cmake runtimes libcxx libcxxabi libunwind third-party $L/ && cp -a llvm/cmake llvm/utils $L/llvm/)
	patch -d $L -p1 < $NQ_TREE/patches/nativeqt-llvm18-runtimes.diff
fi
mkdir -p $B
cmake -G Ninja -S $L/runtimes -B $B \
	-DCMAKE_TOOLCHAIN_FILE=$NQ_TREE/toolchain.cmake \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$NQ_SYSROOT/usr \
	-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	-DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
	-DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF \
	-DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
	-DLIBCXX_ENABLE_EXCEPTIONS=ON -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
	-DLIBCXX_ENABLE_THREADS=ON -DLIBCXXABI_ENABLE_THREADS=ON -DLIBUNWIND_ENABLE_THREADS=ON \
	-DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
	-DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=OFF -DLIBCXXABI_ENABLE_STATIC_UNWINDER=OFF \
	-DLIBCXX_USE_COMPILER_RT=ON -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBUNWIND_USE_COMPILER_RT=ON \
	-DLIBCXX_INSTALL_INCLUDE_DIR=$NQ_SYSROOT/usr/include/c++/v1 \
	-DLIBCXX_INSTALL_INCLUDE_TARGET_DIR=$NQ_SYSROOT/usr/include/c++/v1 \
	-DCMAKE_C_FLAGS="" -DCMAKE_CXX_FLAGS=""
ninja -C $B -j$NQ_JOBS
ninja -j$NQ_JOBS -C $B install
# libunwind: for Wasm exceptions it is only Unwind-wasm.c (as Emscripten builds it); its CMake knows no Wasm.
U=$L/libunwind
nq-clang -c -O2 -std=c99 -D_LIBUNWIND_HIDE_SYMBOLS -DNDEBUG -I$U/include -o $B/Unwind-wasm.o $U/src/Unwind-wasm.c
rm -f $NQ_SYSROOT/usr/lib/libunwind.a
$NQ_LLVM/llvm-ar rcs $NQ_SYSROOT/usr/lib/libunwind.a $B/Unwind-wasm.o
cp $U/include/unwind*.h $U/include/__libunwind_config.h $U/include/libunwind.h $NQ_SYSROOT/usr/include/ 2>/dev/null || true
echo runtimes ok
# libnqcrt.a: crt/nqcrt.c (what every program gets from bin/nq-clang).
nq-clang -c -O2 -o $B/nqcrt.o $NQ_TREE/crt/nqcrt.c
nq-clang -c -o $B/cpp_exception.o $NQ_TREE/crt/cpp_exception.S
nq-clang -c -O2 -o $B/nqsjlj.o $NQ_TREE/crt/nqsjlj.c
nq-clang -c -O2 -o $B/nqmain.o $NQ_TREE/crt/nqmain.c
nq-clang -c -o $B/sjlj_tag.o $NQ_TREE/crt/sjlj_tag.S
# The long double comparisons (__eqtf2 and the rest), again: compiler-rt 18 returns `long` from them on
# every LP64 target but AArch64, and on wasm64 that is an i64, where LLVM calls them expecting an i32 (as
# it does everywhere). The mismatch is a trap the first time printf formats a double. Built here with int.
CRT=$NQ_BUILD/comparetf2
rm -rf $CRT && mkdir -p $CRT && cp $LW/src/llvm/compiler-rt/lib/builtins/{comparetf2.c,fp_compare_impl.inc,fp_lib.h,int_lib.h,int_types.h,int_endianness.h,int_util.h,int_math.h} $CRT/
sed -i 's/^typedef long CMP_RESULT;/typedef int CMP_RESULT;/' $CRT/fp_compare_impl.inc
nq-clang -c -O2 -o $B/comparetf2.o $CRT/comparetf2.c
rm -f $NQ_SYSROOT/usr/lib/libnqcrt.a
$NQ_LLVM/llvm-ar rcs $NQ_SYSROOT/usr/lib/libnqcrt.a $B/nqcrt.o $B/cpp_exception.o $B/nqsjlj.o $B/sjlj_tag.o $B/comparetf2.o $B/nqmain.o
echo nqcrt ok
