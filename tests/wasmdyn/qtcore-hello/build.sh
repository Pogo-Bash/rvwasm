#!/bin/bash
# Build qtcore-hello in the build VM (branch nqshared's shared Qt + branch wasmdyn's ld-wasm):
#   qtcore-hello      the program, libQt6Core.so as NEEDED (a PIE like tests/wasmdyn's main1), C library = ld-wasm's
#   qtcore-hello-static   the same source, nq-clang++ and the static libQt6Core.a (size comparison)
# Output in ../out/ (run.sh fetches it). Needs ../out/ld-wasm, tests/wasmdyn/env.sh.
set -e
cd "$(dirname "$0")"
. ../env.sh
O=../out
NQ=${NQ:-$HOME/rvwasm/nativeqt}
NQS=${NQS:-$HOME/wt/nqshared/shared}
Q=$NQ/build-shared/qtbase
SYS=$NQ/sysroot-shared
LD=$NQ_LLVM/wasm-ld
QINC=(-I$Q/include -I$Q/include/QtCore -I$Q/include/QtCore/6.8.2 -I$Q/include/QtCore/6.8.2/QtCore)
mkdir -p $O/qt
$NQ/host/libexec/moc -I$Q/include -I$Q/include/QtCore main.cpp -o $O/qt/main.moc
# Position independent, the shared sysroot's C++ headers (ld-wasm provides libc++ at run time).
NQ_SYSROOT=$SYS $NQS/bin/nqs-clang++ -O2 -std=c++17 -DQT_NO_DEBUG "${QINC[@]}" -I$O/qt -I. -c main.cpp -o $O/qt/main.o
L=$SYS/usr/lib
PFLAGS=(-mwasm64 --experimental-pic --shared-memory --max-memory=17179869184 --no-entry -pie --import-memory --import-table
	--export-if-defined=__main_argc_argv --export-if-defined=__main_void --export=__wasm_call_ctors --export=__wasm_apply_data_relocs)
(cd $O/qt && cp $Q/lib/libQt6Core.so.6.8.2 libQt6Core.so.6 && $LD "${PFLAGS[@]}" main.o ../dlcrt.o $L/libunwind.a libQt6Core.so.6 -o ../qtcore-hello 2> >(grep -v 'creating shared' >&2))
python3 ../tablify.py --dynsym $O/qtcore-hello > /dev/null
cp $O/qt/libQt6Core.so.6 $O/libQt6Core.so.6
ls -la $O/qtcore-hello $O/libQt6Core.so.6
# The same source as one static program: nq-clang++ and the static Qt (sysroot), for the size comparison.
S=$NQ/sysroot; I=$S/usr/include/qt6
~/wt/nativeqt/nativeqt/bin/nq-clang++ -O2 -std=c++17 -DQT_NO_DEBUG -I$I -I$I/QtCore -I$O/qt -c main.cpp -o $O/qt/main-static.o
~/wt/nativeqt/nativeqt/bin/nq-clang++ -O2 -o $O/qtcore-hello-static $O/qt/main-static.o -L$S/usr/lib -lQt6Core -lQt6BundledPcre2 -lQt6BundledZLIB -lz
ls -la $O/qtcore-hello-static
