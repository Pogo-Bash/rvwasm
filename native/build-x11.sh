#!/bin/bash
# The X11 client libraries (Debian trixie's versions), static, into the sysroot: KWindowSystem's X11 half
# (KX11Extras, NETWM), libplasma and plasma-workspace are written with them in, as Debian builds them. Under
# Wayland none of it runs (KWindowSystem::isPlatformX11() is false); with Xwayland, it could.
#   build-x11.sh [NAME...]   (default: all, in order; a finished one is skipped, NQ_REDO=1 again)
set -e
. "$(dirname "$0")/env.sh"
. $NQ_TREE/meson-cross.sh
export NQ_LAUNCHER=
X=https://xorg.freedesktop.org/releases/individual
XCB=https://xcb.freedesktop.org/dist
declare -A url=(
	[xorgproto]=$X/proto/xorgproto-2024.1.tar.xz
	[xcb-proto]=$XCB/xcb-proto-1.17.0.tar.xz
	[xtrans]=$X/lib/xtrans-1.5.2.tar.xz
	[libXau]=$X/lib/libXau-1.0.11.tar.xz
	[libXdmcp]=$X/lib/libXdmcp-1.1.5.tar.xz
	[libxcb]=$XCB/libxcb-1.17.0.tar.xz
	[libX11]=$X/lib/libX11-1.8.12.tar.xz
	[libXext]=$X/lib/libXext-1.3.6.tar.xz
	[libXfixes]=$X/lib/libXfixes-6.0.1.tar.xz
	[libXrender]=$X/lib/libXrender-0.9.12.tar.xz
	[xcb-util]=$XCB/xcb-util-0.4.1.tar.xz
	[xcb-util-keysyms]=$XCB/xcb-util-keysyms-0.4.1.tar.xz
	[xcb-util-wm]=$XCB/xcb-util-wm-0.4.2.tar.xz
	[xcb-util-renderutil]=$XCB/xcb-util-renderutil-0.3.10.tar.xz
	[xcb-util-image]=$XCB/xcb-util-image-0.4.1.tar.xz
	[xcb-util-cursor]=$XCB/xcb-util-cursor-0.1.5.tar.xz
	[libICE]=$X/lib/libICE-1.1.1.tar.xz
	[libSM]=$X/lib/libSM-1.2.6.tar.xz
	[libXcursor]=$X/lib/libXcursor-1.2.3.tar.xz
	[libXi]=$X/lib/libXi-1.8.2.tar.xz
	[libXtst]=$X/lib/libXtst-1.2.5.tar.xz
	[freetype]=https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
	[expat]=https://github.com/libexpat/libexpat/releases/download/R_2_7_1/expat-2.7.1.tar.xz
	[fontconfig]=https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.xz
	[libXft]=$X/lib/libXft-2.3.8.tar.xz
)
order="xorgproto xcb-proto xtrans libXau libXdmcp libxcb libX11 libXext libXfixes libXrender xcb-util
	xcb-util-keysyms xcb-util-wm xcb-util-renderutil xcb-util-image xcb-util-cursor
"
# (ICE, SM, Xcursor, Xtst and Xft, with freetype and fontconfig: what plasma-workspace's X11 check asks for;
# `build-x11.sh session`.)
session="libICE libSM libXcursor libXi libXtst freetype expat fontconfig libXft"
[ "$1" = session ] && set -- $session
export PKG_CONFIG_SYSROOT_DIR=$NQ_SYSROOT PKG_CONFIG_LIBDIR=$NQ_SYSROOT/usr/lib/pkgconfig:$NQ_SYSROOT/usr/share/pkgconfig
py=$(echo $NQ_SYSROOT/usr/lib/python3*/site-packages)
for n in ${*:-$order}; do
	S=$NQ_SRC/x11/$n
	B=$NQ_BUILD/x11-$n
	[ -e $B/.nq-installed ] && [ -z "$NQ_REDO" ] && continue
	t0=$(date +%s)
	mkdir -p $NQ_SRC/x11 $NQ_SRC/dl
	if [ ! -d $S ]; then
		f=$NQ_SRC/dl/$(basename ${url[$n]})
		[ -s $f ] || curl -sfL -o $f ${url[$n]}
		mkdir -p $S && tar xf $f -C $S --strip-components=1
	fi
	rm -rf $B && mkdir -p $B
	if [ $n = xorgproto ]; then
		meson setup $B $S --cross-file $NQ_CROSS --prefix=/usr -Dlegacy=false
		DESTDIR=$NQ_SYSROOT ninja -C $B install > /dev/null
	elif [ $n = freetype ]; then
		meson setup $B $S --cross-file $NQ_CROSS --prefix=/usr --libdir=lib -Dbrotli=disabled -Dbzip2=disabled \
			-Dharfbuzz=disabled -Dpng=disabled -Dzlib=enabled -Dtests=disabled
		ninja -C $B -j$NQ_JOBS && DESTDIR=$NQ_SYSROOT ninja -C $B install > /dev/null
	elif [ $n = fontconfig ]; then
		meson setup $B $S --cross-file $NQ_CROSS --prefix=/usr --libdir=lib --sysconfdir=/etc --localstatedir=/var \
			-Ddoc=disabled -Dtests=disabled -Dtools=disabled -Dcache-build=disabled -Dnls=disabled
		# The library (and its headers and .pc); the configuration files are the disk's own (Debian's).
		ninja -C $B -j$NQ_JOBS && DESTDIR=$NQ_SYSROOT meson install -C $B --no-rebuild --tags devel,runtime > /dev/null
	else
		# Autotools, cross (the build VM is aarch64; wasm64 is no triplet config.sub knows, and a
		# cross build is what matters: nothing is run).
		(cd $B && CC=nq-clang AR=$NQ_LLVM/llvm-ar RANLIB=$NQ_LLVM/llvm-ranlib CFLAGS="-O2" CC_FOR_BUILD=gcc \
			XCBPROTO_XCBINCLUDEDIR=$NQ_SYSROOT/usr/share/xcb XCBPROTO_XCBPYTHONDIR=$py PYTHONPATH=$py \
			$S/configure --host=x86_64-linux-musl --build=aarch64-linux-gnu --prefix=/usr --libdir=/usr/lib \
			--enable-static --disable-shared --disable-docs --disable-specs --without-xmlto --without-fop \
			--without-xsltproc --disable-devel-docs --disable-malloc0returnsnull --disable-xf86bigfont \
			--disable-loadable-i18n --disable-loadable-xcursor --enable-xthreads --without-libuuid \
			--without-docbook --without-examples --without-tests)
		make -C $B -j$NQ_JOBS PYTHONPATH=$py
		make -C $B install DESTDIR=$NQ_SYSROOT > /dev/null
		# (libtool's .la files would send later links to /usr/lib of the build VM.)
		rm -f $NQ_SYSROOT/usr/lib/*.la
	fi
	# What a link that names only the library needs after it (bin/nq-clang's nq-deps; the .pc files say it in
	# Libs.private, which builds that do not ask for --static never see).
	mkdir -p $NQ_SYSROOT/usr/lib/nq-deps
	case $n in
	libxcb) for l in xcb $(cd $NQ_SYSROOT/usr/lib && ls libxcb-*.a 2>/dev/null | sed 's/^lib//; s/\.a$//'); do
			echo "-lxcb -lXau -lXdmcp" > $NQ_SYSROOT/usr/lib/nq-deps/$l; done ;;
	libX11) echo "-lxcb -lXau -lXdmcp" > $NQ_SYSROOT/usr/lib/nq-deps/X11
		echo "-lX11 -lxcb -lXau -lXdmcp" > $NQ_SYSROOT/usr/lib/nq-deps/X11-xcb ;;
	libXext | libXfixes | libXrender | libXi | libXcursor) echo "-lXrender -lXfixes -lXext -lX11 -lxcb -lXau -lXdmcp" > $NQ_SYSROOT/usr/lib/nq-deps/${n#lib} ;;
	libXtst) echo "-lXi -lXext -lX11 -lxcb -lXau -lXdmcp" > $NQ_SYSROOT/usr/lib/nq-deps/Xtst ;;
	libSM) echo "-lICE" > $NQ_SYSROOT/usr/lib/nq-deps/SM ;;
	fontconfig) echo "-lfreetype -lexpat -lz" > $NQ_SYSROOT/usr/lib/nq-deps/fontconfig ;;
	freetype) echo "-lz" > $NQ_SYSROOT/usr/lib/nq-deps/freetype ;;
	libXft) echo "-lfontconfig -lfreetype -lexpat -lz -lXrender -lX11 -lxcb -lXau -lXdmcp" > $NQ_SYSROOT/usr/lib/nq-deps/Xft ;;
	esac
	touch $B/.nq-installed
	echo "x11 $n: $(( $(date +%s) - t0 )) s"
done
