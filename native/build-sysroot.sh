#!/bin/bash
# The sysroot: linux-wasm's musl and kernel headers (copied; the shared install is not touched), under /usr.
set -e
. "$(dirname "$0")/env.sh"
mkdir -p $NQ_SYSROOT/usr
cp -a $LW/install/musl-wasm64_nommu/include $LW/install/musl-wasm64_nommu/lib $NQ_SYSROOT/usr/
cp -a $LW/install/kernel-wasm64_nommu/include/. $NQ_SYSROOT/usr/include/
python3 $NQ_TREE/sysroot-syscall.py $NQ_SYSROOT/usr/include/unistd.h
echo sysroot ok
