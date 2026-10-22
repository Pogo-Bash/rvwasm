#!/bin/bash
# Qt 6.8.2 as SHARED libraries (dylink.0 side modules) for the in-process dynamic linker of tests/wasmdyn.
# Beside the static build, not in it: $NQ/build-shared/<module>, $NQ/sysroot-shared (shared/prep-sysroot.sh), the
# static build's patched sources $NQ/src (read-only here), host tools $NQ/host.
#   shared/build-qt-shared.sh [modules...]      (default: qtbase qtwayland)
# The install prefix is /usr (what the machine's root file system has: the plugin path Qt bakes in is
# /usr/lib/qt6/plugins); files are staged into $NQ_SYSROOT/usr (CMAKE_STAGING_PREFIX).
# qtbase: Core Gui Widgets Network DBus Xml Concurrent PrintSupport Test (KCrash wants it); Wayland platform only (no xcb), no OpenGL/EGL
# yet (Mesa is static in the static build; a shared EGL comes with Quick), no Sql.
set -e
. "$(dirname "$0")/env.sh"
export NQ_LAUNCHER=ccache
mods=${*:-qtbase qtwayland}
REF=${NQS_LD_WASM:-$NQ/ref/ld-wasm}     # a copy of the prototype's ld-wasm (tests/wasmdyn/out/ld-wasm): the C library's exports
for m in $mods; do
	B=$NQ_BUILD/$m
	t0=$(date +%s)
	if [ ! -e $B/build.ninja ] || [ -n "$NQ_REDO" ]; then
		if [ $m = qtbase ]; then
			cmake -S $NQ_SRC/qtbase -B $B -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NQS_TREE/toolchain.cmake -DQT_HOST_PATH=$NQ/host \
				-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_STAGING_PREFIX=$NQ_SYSROOT/usr -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
				-DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DBUILD_WITH_PCH=OFF -DFEATURE_precompile_header=OFF \
				-DINSTALL_BINDIR=lib/qt6/bin -DINSTALL_LIBEXECDIR=lib/qt6/libexec -DINSTALL_ARCHDATADIR=lib/qt6 \
				-DINSTALL_DATADIR=share/qt6 -DINSTALL_MKSPECSDIR=lib/qt6/mkspecs -DINSTALL_INCLUDEDIR=include/qt6 \
				-DFEATURE_gui=ON -DFEATURE_widgets=ON -DFEATURE_network=ON -DFEATURE_dbus=ON -DFEATURE_dbus_linked=ON \
				-DFEATURE_xml=ON -DFEATURE_concurrent=ON -DFEATURE_printsupport=ON -DFEATURE_sql=OFF -DFEATURE_testlib=ON \
				-DINPUT_opengl=no -DFEATURE_opengl=OFF -DFEATURE_egl=OFF -DFEATURE_eglfs=OFF -DFEATURE_vulkan=OFF -DFEATURE_xcb=OFF -DFEATURE_xlib=OFF \
				-DFEATURE_glib=OFF -DFEATURE_icu=OFF -DFEATURE_cups=OFF -DFEATURE_libproxy=OFF -DFEATURE_evdev=OFF -DFEATURE_libinput=OFF \
				-DFEATURE_tslib=OFF -DFEATURE_mtdev=OFF -DFEATURE_gtk3=OFF -DFEATURE_fontconfig=ON -DFEATURE_system_freetype=ON \
				-DFEATURE_system_harfbuzz=OFF -DFEATURE_system_png=OFF -DFEATURE_system_jpeg=OFF -DFEATURE_system_pcre2=OFF \
				-DFEATURE_system_doubleconversion=OFF -DFEATURE_system_libb2=OFF -DFEATURE_system_zlib=OFF \
				$( [ -e $NQ_SYSROOT/usr/lib/libssl.a ] && echo -DINPUT_openssl=linked -DFEATURE_openssl_linked=ON ) \
				-DFEATURE_reduce_relocations=OFF -DFEATURE_relocatable=OFF -DFEATURE_rpath=OFF -DFEATURE_library=ON \
				-DFEATURE_mimetype_database=ON -DQT_QPA_DEFAULT_PLATFORM=wayland
		else
			mkdir -p $B
			extra=()
			[ $m = qtwayland ] && extra+=(-DFEATURE_wayland_server=OFF)
			# QtSvg's gzip support calls inflate(): Core's bundled zlib is hidden, so Svg carries libz.a (as Gui does).
			[ $m = qtsvg ] && extra+=(-DCMAKE_CXX_STANDARD_LIBRARIES=-lz)
			# QML without the V4 JIT (Wasm cannot make code at run time), as the static build.
			[ $m = qtdeclarative ] && extra+=(-DFEATURE_qml_jit=OFF -DFEATURE_qml_debug=OFF)
			extra+=($NQ_QT_EXTRA)
			$NQ_SYSROOT/usr/lib/qt6/libexec/qt-cmake-private -S $NQ_SRC/$m -B $B -G Ninja -DQT_HOST_PATH=$NQ/host \
				-DCMAKE_BUILD_TYPE=Release -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DBUILD_WITH_PCH=OFF \
				-DCMAKE_SKIP_RPATH=ON "${extra[@]}"
		fi
	fi
	echo "$m configure: $(( $(date +%s) - t0 )) s"
	ninja -C $B -j$NQ_JOBS $NQS_NINJA_FLAGS
	echo "$m build: $(( $(date +%s) - t0 )) s"
	ninja -C $B -j$NQ_JOBS install > $B/install.log
	touch $B/.nq-installed
	echo "$m installed: $(( $(date +%s) - t0 )) s"
done
# Checks: every module against the C library's exports and the Qt libraries themselves.
if [ -e $REF ]; then
	prov=(--provider $REF)
	for f in $NQ_SYSROOT/usr/lib/libQt6*.so.6.8.2 $NQ_SYSROOT/usr/lib/libwayland-client.so.0 $NQ_SYSROOT/usr/lib/libxkbcommon.so.0; do [ -e $f ] && prov+=(--provider $f); done
	for f in $NQ_SYSROOT/usr/lib/libQt6*.so.6.8.2 $(find $NQ_SYSROOT/usr/lib/qt6/plugins -name '*.so' 2>/dev/null); do
		$NQS_TREE/check-dylib.py $f "${prov[@]}" || true
	done
fi
