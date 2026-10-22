#!/bin/bash
# Build qtwidgets-hello in the build VM (branch nqshared's shared Qt in sysroot-shared + branch wasmdyn's ld-wasm):
# a PIE with libQt6Widgets/Gui/Core.so.6 as NEEDED, C library = ld-wasm's. Output ../out/qtwidgets-hello and the libraries.
set -e
cd "$(dirname "$0")"
. ../env.sh
O=../out
NQ=${NQ:-$HOME/rvwasm/nativeqt}
NQS=${NQS:-$HOME/wt/nqshared/shared}
SYS=$NQ/sysroot-shared
I=$SYS/usr/include/qt6
LD=$NQ_LLVM/wasm-ld
mkdir -p $O/qtw
$NQ/host/libexec/moc -I$I -I$I/QtCore -I$I/QtGui -I$I/QtWidgets main.cpp -o $O/qtw/main.moc
NQ_SYSROOT=$SYS $NQS/bin/nqs-clang++ -O2 -std=c++17 -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB \
	-I$I -I$I/QtCore -I$I/QtGui -I$I/QtWidgets -I$O/qtw -I. -c main.cpp -o $O/qtw/main.o
L=$SYS/usr/lib
PFLAGS=(-mwasm64 --experimental-pic --shared-memory --max-memory=17179869184 --no-entry -pie --import-memory --import-table
	--export-if-defined=__main_argc_argv --export-if-defined=__main_void --export=__wasm_call_ctors --export=__wasm_apply_data_relocs)
A=$(cd $O && pwd)
(cd $L && $LD "${PFLAGS[@]}" $A/qtw/main.o $A/dlcrt.o $L/libunwind.a \
	libQt6Widgets.so.6 libQt6Gui.so.6 libQt6Core.so.6 -o $A/qtwidgets-hello 2> >(grep -v 'creating shared' >&2))
python3 ../tablify.py --dynsym $O/qtwidgets-hello > /dev/null
ls -la $O/qtwidgets-hello
