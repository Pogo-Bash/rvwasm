#!/bin/bash
# Qt 6.8.2 for the build VM itself (arm64 Linux): the host tools a cross build needs (moc, rcc, uic,
# qsb, qmlcachegen, qmltyperegistrar, qtwaylandscanner, ...), installed in $NQ/host (QT_HOST_PATH).
# Modules: as arguments (default: qtbase qtshadertools qtsvg qtdeclarative qtwayland; qtsvg before
# qtdeclarative, whose svgtoqml a cross build of qtdeclarative needs once the target has QtSvg); a finished one is skipped.
set -e
. "$(dirname "$0")/env.sh"
H=$NQ/host
mods=${*:-qtbase qtshadertools qtsvg qtdeclarative qtwayland}
$NQ_TREE/qt-prep.sh $mods
for m in $mods; do
	[ -e $NQ_BUILD/host-$m/.nq-installed ] && continue
	t0=$(date +%s)
	if [ $m = qtbase ]; then
		cmake -G Ninja -S $NQ_SRC/qtbase -B $NQ_BUILD/host-qtbase -DCMAKE_INSTALL_PREFIX=$H \
			-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
			-DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DINPUT_opengl=no -DFEATURE_xcb=OFF -DFEATURE_xlib=OFF \
			-DFEATURE_sql=OFF -DFEATURE_printsupport=OFF -DFEATURE_widgets=OFF -DFEATURE_dbus=ON -DFEATURE_dbus_linked=OFF \
			-DFEATURE_glib=OFF -DFEATURE_icu=OFF -DFEATURE_fontconfig=OFF \
			-DFEATURE_system_freetype=OFF -DFEATURE_system_harfbuzz=OFF -DFEATURE_system_png=OFF \
			-DFEATURE_system_jpeg=OFF -DFEATURE_system_pcre2=OFF -DFEATURE_system_doubleconversion=OFF \
			-DFEATURE_system_libb2=OFF -DFEATURE_libproxy=OFF -DFEATURE_vulkan=OFF
	else
		mkdir -p $NQ_BUILD/host-$m
		(cd $NQ_BUILD/host-$m && $H/bin/qt-configure-module $NQ_SRC/$m -- -DCMAKE_C_COMPILER_LAUNCHER=ccache \
			-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF)
	fi
	ninja -C $NQ_BUILD/host-$m -j$NQ_JOBS
	ninja -j$NQ_JOBS -C $NQ_BUILD/host-$m install > /dev/null
	touch $NQ_BUILD/host-$m/.nq-installed
	echo "host $m: $(( $(date +%s) - t0 )) s"
done
