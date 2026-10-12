#!/bin/bash
# A C library some KDE part needs, static, into the sysroot (with its CMake or pkg-config files):
#   build-clib.sh NAME cmake|meson [OPTIONS...]     sources in $NQ_SRC/NAME (fetch-kde.sh NAME)
# A finished one is skipped (its marker; NQ_REDO=1: again).
set -e
. "$(dirname "$0")/env.sh"
n=$1 kind=$2
shift 2
S=$NQ_SRC/$n
B=$NQ_BUILD/clib-$n
[ -e $B/.nq-installed ] && [ -z "$NQ_REDO" ] && { echo "$n: built already"; exit 0; }
export NQ_LAUNCHER=ccache
case $kind in
cmake)
	cmake -S $S -B $B -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NQ_TREE/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF "$@"
	ninja -C $B -j$NQ_JOBS
	# (NQ_CLIB_INSTALL: what to do instead of "ninja install", for a project that has no install target:
	# run with $B and $S set, e.g. libdmtx's library and header copied)
	if [ -n "$NQ_CLIB_INSTALL" ]; then
		B=$B S=$S bash -c "$NQ_CLIB_INSTALL"
	else
		DESTDIR=$NQ_SYSROOT ninja -C $B install > /dev/null
	fi
	;;
meson)
	. $NQ_TREE/meson-cross.sh
	meson setup --wipe $B $S --cross-file $NQ_CROSS --prefix=/usr --libdir=lib -Ddefault_library=static "$@"
	ninja -C $B -j$NQ_JOBS
	DESTDIR=$NQ_SYSROOT meson install -C $B --no-rebuild > /dev/null
	;;
*)
	echo "build-clib: cmake or meson, not $kind" >&2
	exit 1
	;;
esac
touch $B/.nq-installed
echo "$n: built"
