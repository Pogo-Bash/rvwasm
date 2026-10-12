#!/bin/bash
# libwayland (client, cursor; 1.23.1 as Debian trixie) without libffi (patches/nativeqt-wayland.diff,
# wayland-noffi.py), and libxkbcommon 1.7.0, static, into the sysroot. The host's wayland-scanner 1.23.1
# goes to $NQ/host.
set -e
. "$(dirname "$0")/env.sh"
. $NQ_TREE/meson-cross.sh
S=$NQ_SRC/wayland
cd $S
git diff --quiet && patch -p1 -s < $NQ_TREE/patches/nativeqt-wayland.diff
python3 $NQ_TREE/wayland-noffi.py 10 > src/closure-noffi.h
if [ ! -x $NQ/host/bin/wayland-scanner ]; then
	meson setup --wipe $NQ_BUILD/host-wayland $S --prefix=$NQ/host -Dlibraries=false -Dscanner=true \
		-Dtests=false -Ddocumentation=false -Ddtd_validation=false > /dev/null
	ninja -j$NQ_JOBS -C $NQ_BUILD/host-wayland install > /dev/null
fi
export PATH=$NQ/host/bin:$PATH
meson setup --wipe $NQ_BUILD/wayland $S --cross-file $NQ_CROSS --prefix=/usr --libdir=lib \
	-Dbuild.pkg_config_path=$NQ/host/lib/aarch64-linux-gnu/pkgconfig \
	-Ddefault_library=static -Dnoffi=true -Dlibraries=true -Dscanner=false -Dtests=false \
	-Ddocumentation=false -Ddtd_validation=false
ninja -j$NQ_JOBS -C $NQ_BUILD/wayland
DESTDIR=$NQ_SYSROOT ninja -j$NQ_JOBS -C $NQ_BUILD/wayland install > /dev/null
# The protocol XML and the scanner's pkg-config file, for Qt's search of them (the scanner is the host's).
mkdir -p $NQ_SYSROOT/usr/share/wayland $NQ_SYSROOT/usr/lib/pkgconfig
cp $S/protocol/wayland.xml $NQ_SYSROOT/usr/share/wayland/
# Its variables made to point where a cross build looks: the XML in the sysroot (CMake's FindWayland adds the
# sysroot itself), the scanner the host's.
sed -e 's|^prefix=.*|prefix=/usr|' -e 's|^pkgdatadir=.*|pkgdatadir=${datarootdir}/wayland|' \
	-e "s|^wayland_scanner=.*|wayland_scanner=$NQ/host/bin/wayland-scanner|" \
	$NQ/host/lib/aarch64-linux-gnu/pkgconfig/wayland-scanner.pc > $NQ_SYSROOT/usr/lib/pkgconfig/wayland-scanner.pc

X=$NQ_SRC/libxkbcommon
# (ours: patches/nativeqt-libxkbcommon.diff, once)
if [ ! -e $X/.nq-patched ]; then
	patch -d $X -p1 -s -N < $NQ_TREE/patches/nativeqt-libxkbcommon.diff
	touch $X/.nq-patched
fi
meson setup --wipe $NQ_BUILD/xkbcommon $X --cross-file $NQ_CROSS --prefix=/usr --libdir=lib \
	-Ddefault_library=static -Denable-x11=$( [ -e $NQ_SYSROOT/usr/lib/libxcb-xkb.a ] && echo true || echo false ) -Denable-wayland=false -Denable-docs=false \
	-Denable-tools=false -Denable-xkbregistry=$( [ -n "$NQ_XKBREGISTRY" ] && echo true || echo false ) -Dxkb-config-root=/usr/share/X11/xkb \
	-Dx-locale-root=/usr/share/X11/locale
# The libraries only: its tests and benchmarks are not wanted (and test-x11comp's harness uses __start_ and
# __stop_ section symbols, which a Wasm link has not got).
ninja -j$NQ_JOBS -C $NQ_BUILD/xkbcommon libxkbcommon.a $( [ -e $NQ_SYSROOT/usr/lib/libxcb-xkb.a ] && echo libxkbcommon-x11.a ) $( [ -n "$NQ_XKBREGISTRY" ] && echo libxkbregistry.a )
DESTDIR=$NQ_SYSROOT meson install -C $NQ_BUILD/xkbcommon --no-rebuild > /dev/null
echo wayland xkbcommon ok
