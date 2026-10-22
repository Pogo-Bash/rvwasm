#!/bin/bash
# GMP 6.3.0 and MPFR 4.2.1 (what KCalc's knumber needs), static and position independent, into sysroot-shared (they
# end up inside the kcalc program). Autotools, cross, nothing is run; GMP without assembly (generic C, 64-bit limbs).
set -e
. "$(dirname "$0")/env.sh"
export NQ_LAUNCHER=ccache
SRC=$NQ/src-shared; mkdir -p $SRC/dl
build() { # name host url extra-configure-args...
	n=$1 h=$2 u=$3; shift 3
	B=$NQ_BUILD/clib-$n
	[ -e $B/.nq-installed ] && [ -z "$NQ_REDO" ] && return
	[ -s $SRC/dl/$(basename $u) ] || curl -sfL -o $SRC/dl/$(basename $u) $u
	[ -d $SRC/$n ] || { mkdir -p $SRC/$n && tar xf $SRC/dl/$(basename $u) -C $SRC/$n --strip-components=1; }
	rm -rf $B && mkdir -p $B
	(cd $B && CC=nq-clang AR=$NQ_LLVM/llvm-ar RANLIB=$NQ_LLVM/llvm-ranlib CFLAGS="-O2" CC_FOR_BUILD=gcc \
		$SRC/$n/configure --host=$h --build=aarch64-linux-gnu --prefix=/usr --libdir=/usr/lib \
		--enable-static --disable-shared "$@")
	make -C $B -j$NQ_JOBS
	make -C $B install DESTDIR=$NQ_SYSROOT > /dev/null
	rm -f $NQ_SYSROOT/usr/lib/*.la
	touch $B/.nq-installed
	echo "$n: built"
}
build gmp none-linux-gnu https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz --disable-assembly --with-pic
build mpfr x86_64-linux-musl https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.1.tar.xz --with-gmp=$NQ_SYSROOT/usr --with-pic
