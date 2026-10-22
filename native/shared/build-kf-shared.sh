#!/bin/bash
# KDE Frameworks 6.13 (and KDE apps) as SHARED libraries into sysroot-shared, in the order given; a finished one
# is skipped (NQ_REDO=1: again). Beside the static build: its own pristine source trees $NQ/src-shared/NAME (from
# the tarballs in $NQ/src/dl, or $NQ/src-shared/dl), builds in $NQ/build-shared/kf-NAME. The static build's
# in-place patched sources are not used: most of its patches are for one static binary (renamed clashing symbols,
# KPluginMetaData that never opens a plugin file). Host tools (kconfig_compiler) are the static build's host ones.
#   shared/build-kf-shared.sh NAME...
# (The plugin and QML directories are given: KDE_INSTALL_USE_QT_SYS_PATHS asks the HOST Qt, and a first run wrote 17 wasm plugins
# into $NQ/host/plugins; moved to sysroot-shared/usr/lib/qt6/plugins, host tree clean again.)
# Options as Debian's dh kf6 (BUILD_TESTING off, KDE_INSTALL_USE_QT_SYS_PATHS) plus: no QML parts (KConfig, KCoreAddons,
# KI18n scripting, KIconThemes' Quick plugin: a Widgets program needs none and QtQml is not built shared yet), no
# BreezeIcons (the icon theme is data on the rootfs), no X11, no Wayland extras in KGuiAddons (needs plasma-wayland-protocols data, not in sysroot-shared), no translations.
set -e
. "$(dirname "$0")/env.sh"
export NQ_LAUNCHER=ccache
opt=${NQ_KDE_OPT:--O2}
SRC=$NQ/src-shared
mkdir -p $SRC/dl
for n in "$@"; do
	B=$NQ_BUILD/kf-$n
	[ -e $B/.nq-installed ] && [ -z "$NQ_REDO" ] && continue
	t0=$(date +%s)
	S=$SRC/$n
	if [ ! -d $S ]; then
		t=$(ls $NQ/src/dl/$n-[0-9]*.tar.* $SRC/dl/$n-[0-9]*.tar.* 2>/dev/null | head -1)
		[ -n "$t" ] || { echo "build-kf-shared: no tarball for $n" >&2; exit 1; }
		mkdir -p $S && tar xf $t -C $S --strip-components=1
		rm -rf $S/po $S/poqm
		# xz's CMake reads po/LINGUAS even with NLS off
		[ $n = xz ] && { mkdir -p $S/po; : > $S/po/LINGUAS; }
		# A CMake project of ours over a tarball that has none (native/shared/extra/NAME/CMakeLists.txt)
		[ -f $NQS_TREE/extra/$n/CMakeLists.txt ] && cp $NQS_TREE/extra/$n/CMakeLists.txt $S/CMakeLists.txt
		# Patches that are not about one static binary: nativeqt-NAME.diff for these, nqshared-NAME.diff for ours.
		for p in $NQ_TREE/patches/nqshared-$n.diff $( case $n in knotifications|solid) echo $NQ_TREE/patches/nativeqt-$n.diff ;; esac ); do
			[ -f $p ] && patch -d $S -p1 -s -N < $p
		done
	fi
	# KArchive reads .tar.xz / .tar.bz2 / .tar.zst (KNewStuff's downloads, Ark) only with xz, bzip2 and zstd: the shared liblzma, libbz2 and libzstd
	# of the sysroot (build-kf-shared.sh xz bzip2 zstd; Ark's libarchive uses the same ones); later -D wins over the OFF defaults below.
	# (OpenSSL stays off: since the TLS plugin the sysroot has libcrypto.a, which KArchive would link whole, 3 MB, for encrypted 7z.)
	extra=""
	[ $n = karchive ] && extra="-DWITH_BZIP2=ON -DWITH_LIBLZMA=ON -DWITH_LIBZSTD=ON -DWITH_OPENSSL=OFF -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON"
	ht=$(f=$(find $NQ/host -name KF6ConfigCompilerTargets.cmake 2>/dev/null | head -1); [ -n "$f" ] && dirname "$(dirname "$f")" || echo $NQ/host/lib/cmake)
	cmake -S $S -B $B -G Ninja -DBUILD_TESTING=OFF -DBUILD_QCH=OFF -DBUILD_DOC=OFF -DBUILD_PYTHON_BINDINGS=OFF \
		-DKDE_INSTALL_USE_QT_SYS_PATHS=ON -DKDE_INSTALL_QTPLUGINDIR=lib/qt6/plugins -DKDE_INSTALL_PLUGINDIR=lib/qt6/plugins -DKDE_INSTALL_QMLDIR=lib/qt6/qml -DKDE_SKIP_TEST_SETTINGS=ON -DBUILD_WITH_QT6=ON -DQT_MAJOR_VERSION=6 \
		-DKCONFIG_USE_QML=OFF -DKCOREADDONS_USE_QML=OFF -DBUILD_WITH_QML=OFF -DKICONTHEMES_USE_QTQUICK=OFF \
		-DWITH_X11=OFF -DWITH_WAYLAND=OFF -DWITH_XCB=OFF -DWITH_BZIP2=OFF -DWITH_LIBLZMA=OFF -DWITH_LIBZSTD=OFF -DUSE_BreezeIcons=OFF \
		-DUDEV_DISABLED=ON -DSONNET_USE_QML=OFF -DSONNET_NO_BACKENDS=ON -DWITH_TEXT_TO_SPEECH=OFF -DBUILD_DESIGNERPLUGIN=OFF \
		-DKWINDOWSYSTEM_QML=OFF -DKWINDOWSYSTEM_X11=OFF -DKWINDOWSYSTEM_WAYLAND=OFF \
		-DCMAKE_TOOLCHAIN_FILE=$NQ_SYSROOT/usr/lib/cmake/Qt6/qt.toolchain.cmake -DQT_HOST_PATH=$NQ/host \
		-DKF6_HOST_TOOLING=$ht -DKF_IGNORE_PLATFORM_CHECK=ON \
		-DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_STAGING_PREFIX=$NQ_SYSROOT/usr -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=None \
		-DCMAKE_SKIP_RPATH=ON -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
		-DCMAKE_C_FLAGS="$opt -DNDEBUG" -DCMAKE_CXX_FLAGS="$opt -DNDEBUG -DQT_NO_DEBUG" $extra $NQS_KF_EXTRA
	ninja -j$NQ_JOBS -k ${NQS_NINJA_K:-1} -C $B
	ninja -C $B install > $B/install.log
	touch $B/.nq-installed
	echo "kf $n: $(( $(date +%s) - t0 )) s"
done
