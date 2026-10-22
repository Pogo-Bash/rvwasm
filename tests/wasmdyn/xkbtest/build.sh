#!/bin/bash
# xkbtest against the shared libxkbcommon.so (build VM). Output ../out/xkbtest.
set -e
cd "$(dirname "$0")"
. ../env.sh
O=../out; A=$(cd $O && pwd)
SYS=${SYS:-$HOME/rvwasm/nativeqt/sysroot-shared}
NQS=${NQS:-$HOME/wt/nqshared/shared}
NQ_SYSROOT=$SYS $NQS/bin/nqs-clang -O2 -I$SYS/usr/include -c xkbtest.c -o $A/xkbtest.o
PFLAGS=(-mwasm64 --experimental-pic --shared-memory --max-memory=17179869184 --no-entry -pie --import-memory --import-table
	--export-if-defined=__main_argc_argv --export-if-defined=__main_void --export=__wasm_call_ctors --export=__wasm_apply_data_relocs)
(cd $SYS/usr/lib && $NQ_LLVM/wasm-ld "${PFLAGS[@]}" $A/xkbtest.o $A/dlcrt.o libxkbcommon.so.0 -o $A/xkbtest 2> >(grep -v 'creating shared' >&2))
python3 ../tablify.py --dynsym $A/xkbtest > /dev/null
ls -la $A/xkbtest
