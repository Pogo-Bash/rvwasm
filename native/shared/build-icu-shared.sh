#!/bin/bash
# ICU 76.1 (Debian trixie's) for Konsole's bidi/shaping (ubidi, ushape): cross-built static and position independent
# (needs a native build of ICU first, for its data tools), then each library linked --whole-archive into one side
# module: libicudata.so, libicuuc.so, libicui18n.so (+ symlinks) in sysroot-shared. The DATA is a separate file
# (--with-data-packaging=archive: usr/share/icu/76.1/icudt76l.dat, mapped at run time), trimmed with the host icupkg to
# no locale/collation/translit/break/converter data (31.8 MB -> 1.1 MB). Konsole needs only bidi/shaping, whose tables
# (uprops, ucase, ubidi, normalization) are compiled into libicuuc itself since ICU 49; the .dat is for the rest.
# (The source tarball has prebuilt data only, so ICU_DATA_FILTER_FILE does nothing; icupkg removes items instead.)
set -e
. "$(dirname "$0")/env.sh"
export NQ_LAUNCHER=ccache
SRC=$NQ/src-shared; mkdir -p $SRC/dl
u=https://github.com/unicode-org/icu/releases/download/release-76-1/icu4c-76_1-src.tgz
[ -s $SRC/dl/icu4c-76_1-src.tgz ] || curl -sfL -o $SRC/dl/icu4c-76_1-src.tgz $u
[ -d $SRC/icu ] || { mkdir -p $SRC/icu && tar xf $SRC/dl/icu4c-76_1-src.tgz -C $SRC/icu --strip-components=1; }
S=$SRC/icu/source
# double-conversion's architecture whitelist lacks wasm64 (it knows __EMSCRIPTEN__ / __wasm32__): doubles are exact on wasm
grep -q '__wasm__' $S/i18n/double-conversion-utils.h || sed -i 's/defined(__EMSCRIPTEN__) || defined(__wasm32__)/defined(__EMSCRIPTEN__) || defined(__wasm__)/' $S/i18n/double-conversion-utils.h
H=$NQ_BUILD/icu-host B=$NQ_BUILD/icu
if [ ! -e $H/.built ]; then
	rm -rf $H; mkdir -p $H
	(cd $H && CC=gcc CXX=g++ $S/configure --disable-tests --disable-samples --disable-extras --disable-layoutex --enable-static --disable-shared)
	make -C $H -j$NQ_JOBS
	touch $H/.built
fi
if [ ! -e $B/.nq-installed ] || [ -n "$NQ_REDO" ]; then
	rm -rf $B; mkdir -p $B
	(cd $B && CC=nq-clang CXX=nq-clang++ AR=$NQ_LLVM/llvm-ar RANLIB=$NQ_LLVM/llvm-ranlib \
		CFLAGS="-O2 -fvisibility=default" CXXFLAGS="-O2 -fvisibility=default -std=c++17" \
		$S/configure --host=x86_64-linux-musl --build=aarch64-linux-gnu --with-cross-build=$H --prefix=/usr --libdir=/usr/lib \
		--enable-static --disable-shared --disable-tools --disable-tests --disable-samples --disable-extras --disable-layoutex \
		--disable-icuio --with-data-packaging=archive)
	make -C $B -j$NQ_JOBS
	make -C $B install DESTDIR=$NQ_SYSROOT > /dev/null
	rm -f $NQ_SYSROOT/usr/lib/*.la
	touch $B/.nq-installed
fi
L=$NQ_SYSROOT/usr/lib
for spec in "icudata:" "icuuc:-licudata" "icui18n:-licuuc -licudata"; do
	n=${spec%%:*}; dep=${spec#*:}
	[ -e $L/lib$n.a ] || continue
	# dependencies first (their .so, not .a): the new library's imports are then NEEDED entries
	$NQS_TREE/bin/nqs-clang++ -shared -o $L/lib$n.so.76.1 -Wl,--whole-archive $L/lib$n.a -Wl,--no-whole-archive \
		$( for d in $dep; do echo $L/lib${d#-l}.so.76.1; done )
	ln -sf lib$n.so.76.1 $L/lib$n.so.76; ln -sf lib$n.so.76.1 $L/lib$n.so
	mkdir -p $L/nqshared-removed && mv $L/lib$n.a $L/nqshared-removed/
	$NQS_TREE/check-dylib.py $L/lib$n.so.76.1 --provider ${NQS_LD_WASM:-$NQ/ref/ld-wasm} $( for d in $dep; do echo --provider $L/lib${d#-l}.so.76.1; done )
done
# data: trim the installed archive (item names are matched without the tree prefix; the file must be called icudt76l.dat)
D=$NQ_SYSROOT/usr/share/icu/76.1
if [ $(stat -c %s $D/icudt76l.dat) -gt 4000000 ]; then
	T=$(mktemp -d); I=$H/bin/icupkg
	cp $D/icudt76l.dat $T/; (cd $T
	printf "%s\n" "coll/*" "curr/*" "lang/*" "region/*" "unit/*" "zone/*" "rbnf/*" "brkitr/*" "translit/*" > r1.txt
	$I --ignore-deps -r r1.txt icudt76l.dat a.dat
	# top-level locale resources and converters: everything but the root bundle and the little tables it needs
	$I -l a.dat | grep -E "\.(cnv|cfu|spp)$|^(metaZones|windowsZones|zoneinfo64|timezoneTypes|nfkc_cf|nfkc_scf|uemoji|ulayout)" > r2.txt
	$I -l a.dat | grep "\.res$" | grep -vE "^(root|res_index|pool|icustd|keyTypeData|supplementalData|numberingSystems|plurals|likelySubtags)\.res$" >> r2.txt
	$I --ignore-deps -r r2.txt a.dat b.dat; cp b.dat $D/icudt76l.dat)
	rm -rf $T
fi
rm -rf $D/config $D/install-sh $D/mkinstalldirs
ls -l $D/*.dat
