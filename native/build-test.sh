#!/bin/bash
# Build a test program from native/tests/NAME (a CMake project) against the sysroot's Qt.
#   build-test.sh NAME   -> $NQ/build/test-NAME/NAME-test, copied to $NQ/out/
set -e
. "$(dirname "$0")/env.sh"
export NQ_LAUNCHER=ccache
n=$1
B=$NQ_BUILD/test-$n
$NQ_SYSROOT/usr/lib/qt6/bin/qt-cmake -S $NQ_TREE/tests/$n -B $B -G Ninja -DQT_HOST_PATH=$NQ/host \
	-DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 || $NQ_SYSROOT/usr/lib/qt6/libexec/qt-cmake-private -S $NQ_TREE/tests/$n -B $B -G Ninja \
	-DQT_HOST_PATH=$NQ/host -DCMAKE_BUILD_TYPE=Release
rm -f $B/$n-test   # always relink: ninja does not see changes to bin/nq-clang or the crt
ninja -j$NQ_JOBS -C $B
mkdir -p $NQ/out
cp $B/$n-test $NQ/out/
ls -la $NQ/out/$n-test
