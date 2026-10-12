#!/bin/bash
# Qt 6.8.2 for native linux-wasm programs: static, installed into the sysroot (/usr there), host tools
# (No precompiled headers: ccache served one made with other target features. OpenGL: ES 2 through Mesa's EGL (build-mesa.sh); Debian's riscv64 Qt uses desktop GL through libglvnd.)
# from $NQ/host. Modules as arguments (default: qtbase qtwayland); a finished one is skipped
# (NQ_REDO=1: configure and build again, incrementally).
set -e
. "$(dirname "$0")/env.sh"
export NQ_LAUNCHER=ccache
mods=${*:-qtbase qtwayland}
$NQ_TREE/qt-prep.sh $mods
common=(-G Ninja -DCMAKE_TOOLCHAIN_FILE=$NQ_TREE/toolchain.cmake -DQT_HOST_PATH=$NQ/host
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$NQ_SYSROOT/usr -DBUILD_SHARED_LIBS=OFF
	-DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DBUILD_WITH_PCH=OFF -DFEATURE_precompile_header=OFF)
for m in $mods; do
	B=$NQ_BUILD/$m
	[ -e $B/.nq-installed ] && [ -z "$NQ_REDO" ] && continue
	t0=$(date +%s)
	if [ $m = qtbase ]; then
		cmake -S $NQ_SRC/qtbase -B $B "${common[@]}" \
			-DINSTALL_BINDIR=lib/qt6/bin -DINSTALL_LIBEXECDIR=lib/qt6/libexec -DINSTALL_ARCHDATADIR=lib/qt6 \
			-DINSTALL_DATADIR=share/qt6 -DINSTALL_MKSPECSDIR=lib/qt6/mkspecs -DINSTALL_INCLUDEDIR=include/qt6 \
			-DINPUT_opengl=es2 -DFEATURE_opengles2=ON -DFEATURE_egl=ON -DFEATURE_eglfs=OFF $( [ -e $NQ_SYSROOT/usr/lib/libxkbcommon-x11.a ] && echo -DFEATURE_xcb=ON -DFEATURE_xlib=ON -DFEATURE_xcb_xlib=ON -DFEATURE_xkbcommon_x11=ON || echo -DFEATURE_xcb=OFF -DFEATURE_xlib=OFF ) -DFEATURE_vulkan=OFF \
			-DFEATURE_dbus=ON -DFEATURE_dbus_linked=ON $( [ -e $NQ_SYSROOT/usr/lib/libssl.a ] && echo -DINPUT_openssl=linked -DFEATURE_openssl_linked=ON ) -DFEATURE_glib=OFF -DFEATURE_icu=OFF -DFEATURE_fontconfig=ON \
			-DFEATURE_sql=ON -DFEATURE_sql_sqlite=ON -DFEATURE_system_sqlite=ON -DFEATURE_sql_mysql=OFF -DFEATURE_sql_psql=OFF -DFEATURE_sql_odbc=OFF -DFEATURE_sql_ibase=OFF -DFEATURE_cups=OFF -DFEATURE_libproxy=OFF -DFEATURE_evdev=OFF -DFEATURE_libinput=OFF \
			-DFEATURE_tslib=OFF -DFEATURE_mtdev=OFF -DFEATURE_system_freetype=ON -DFEATURE_system_harfbuzz=OFF \
			-DFEATURE_system_png=OFF -DFEATURE_system_jpeg=OFF -DFEATURE_system_pcre2=OFF \
			-DFEATURE_system_doubleconversion=OFF -DFEATURE_system_libb2=OFF -DFEATURE_system_zlib=OFF \
			-DFEATURE_reduce_relocations=OFF -DFEATURE_relocatable=OFF -DFEATURE_rpath=OFF \
			-DFEATURE_mimetype_database=ON -DQT_QPA_DEFAULT_PLATFORM=wayland
	else
		mkdir -p $B
		extra=()
		# QML without the V4 JIT (Wasm cannot make code at run time; the interpreter and AOT-compiled QML).
		[ $m = qtdeclarative ] && extra+=(-DFEATURE_qml_jit=OFF -DFEATURE_qml_debug=OFF)
		# The client side only: programs here are Wayland clients of wlserver.
		[ $m = qtwayland ] && extra+=(-DFEATURE_wayland_server=OFF)
		# (NQ_QT_EXTRA: more configure options for this module, e.g. Qt Multimedia without media backends)
		extra+=($NQ_QT_EXTRA)
		$NQ_SYSROOT/usr/lib/qt6/libexec/qt-cmake-private -S $NQ_SRC/$m -B $B -G Ninja -DQT_HOST_PATH=$NQ/host \
			-DCMAKE_BUILD_TYPE=Release -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DBUILD_WITH_PCH=OFF -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
			-DCMAKE_SKIP_RPATH=ON "${extra[@]}"
	fi
	ninja -C $B -j$NQ_JOBS $NQ_NINJA_FLAGS
	ninja -j$NQ_JOBS -C $B install > /dev/null
	touch $B/.nq-installed
	echo "target $m: $(( $(date +%s) - t0 )) s"
done
