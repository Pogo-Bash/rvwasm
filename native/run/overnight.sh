#!/bin/bash
# The overnight build, with no agent attached: everything from libdbus to plasmashell, in dependency order,
# at -O1 (NQ_KDE_OPT), -j12 with a load limit. A module that fails is recorded and only the modules that need
# it are skipped. Each module's log is logs/overnight/NAME.log (ninja -k 0: all of a module's compile errors);
# the summary, the only thing to read in the morning, is logs/overnight-summary.txt:
#   module  ok|FAILED|skipped  minutes  first error line (or the failed dependency)
# Start (detached, survives the agent and the session):
#   setsid nohup bash native/run/overnight.sh > /dev/null 2>&1 < /dev/null &
# Again later: modules already installed are skipped (their markers), failed ones are tried again.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
export NQ_JOBS=12 NQ_NINJA_FLAGS="-k 0 -l 13" NQ_KDE_OPT=${NQ_KDE_OPT:--O1}
L=$NQ/logs/overnight
S=$NQ/logs/overnight-summary.txt
mkdir -p $L
T=$NQ_TREE
printf "%-28s %-8s %7s  %s\n" module status minutes "first error / reason" > $S
echo "# started $(date '+%F %T'); -j$NQ_JOBS $NQ_NINJA_FLAGS, $NQ_KDE_OPT" >> $S
declare -A status

# step NAME "DEPS" COMMAND...: run COMMAND unless a dependency did not end ok.
step() {
	local name=$1 deps=$2 d
	shift 2
	for d in $deps; do
		if [ "${status[$d]}" != ok ]; then
			status[$name]=skipped
			printf "%-28s %-8s %7s  %s\n" "$name" skipped - "needs $d (${status[$d]:-not run})" >> $S
			return
		fi
	done
	# Less parallel when memory is short (a big link next to compiles).
	local avail=$(awk '/MemAvailable/ {print int($2 / 1024)}' /proc/meminfo)
	if [ "$avail" -lt 2048 ]; then export NQ_JOBS=8; else export NQ_JOBS=12; fi
	local t0=$(date +%s)
	echo "=== $name: $* ($(date '+%T'), $avail MiB free, -j$NQ_JOBS)" > $L/$name.log
	if "$@" >> $L/$name.log 2>&1; then
		status[$name]=ok
		printf "%-28s %-8s %7s\n" "$name" ok $(( ($(date +%s) - t0 + 30) / 60 )) >> $S
	else
		status[$name]=FAILED
		local e=$(grep -m 1 -E "error:|undefined symbols|CMake Error" $L/$name.log | cut -c1-200)
		[ -z "$e" ] && e=$(grep -m 1 -E "Error|FAILED|not found|No such file" $L/$name.log | cut -c1-200)
		printf "%-28s %-8s %7s  %s\n" "$name" FAILED $(( ($(date +%s) - t0 + 30) / 60 )) "$e" >> $S
	fi
}

# --- sources (network; each already there is skipped)
step fetch "" $T/fetch-kde.sh dbus zlib sqlite openssl boost qt5compat extra-cmake-modules plasma-wayland-protocols plasma5support kirigami-addons kquickcharts plasma-integration qtimageformats attica knewstuff ktextwidgets kholidays libksysguard kwin

# --- the C runtime pieces (crt/), then the host side:
step runtimes "" $T/build-runtimes.sh
# host: Qt with QtDBus (qdbusxml2cpp), ECM, kconfig's compiler
step host-qtbase-dbus "" bash -c "[ -e $NQ_BUILD/host-qtbase/.nq-dbus ] || { rm -f $NQ_BUILD/host-qtbase/.nq-installed; $T/build-host-qt.sh qtbase && touch $NQ_BUILD/host-qtbase/.nq-dbus; }"
step host-ecm "" $T/build-kf.sh -host extra-cmake-modules
step host-kconfig "host-qtbase-dbus host-ecm" env NQ_KF_EXTRA="-DUSE_DBUS=OFF -DKCONFIG_USE_DBUS=OFF -DKCONFIG_USE_QML=OFF" \
	$T/build-kf.sh -host kconfig
# (kpackagetool6, which libplasma runs at build time to install its packages, and what it needs)
step host-kpackage "host-qtbase-dbus host-ecm" env NQ_KF_EXTRA="-DWITH_BZIP2=OFF -DWITH_LIBLZMA=OFF -DWITH_LIBZSTD=OFF -DWITH_OPENSSL=OFF -DBUILD_WITH_QML=OFF -DUSE_DBUS=OFF -DKCOREADDONS_USE_QML=OFF" \
	$T/build-kf.sh -host karchive kcoreaddons ki18n kpackage
step host-kcmutils "host-qtbase-dbus host-ecm" env NQ_KF_EXTRA="-DTOOLS_ONLY=ON" $T/build-kf.sh -host kcmutils

# --- C libraries
step zlib fetch bash -c ". $T/env.sh; cd $NQ_SRC/zlib && make distclean > /dev/null 2>&1; \
	CC=nq-clang AR=\$NQ_LLVM/llvm-ar RANLIB=\$NQ_LLVM/llvm-ranlib CFLAGS='-O2' ./configure --static --prefix=/usr && \
	make -j\$NQ_JOBS libz.a && make install DESTDIR=\$NQ_SYSROOT && \
	mkdir -p \$NQ_SYSROOT/usr/lib/nq-deps && echo -lz > \$NQ_SYSROOT/usr/lib/nq-deps/Qt6Svg"
# (QtSvg, built again with a zlib in the sysroot, takes it, and its CMake package does not pass -lz on.)
step dbus fetch $T/build-dbus.sh
# The X11 client libraries, and xkbcommon again with its X11 half: Qt's xcb support (QtGui's X11 extras,
# which KWindowSystem's X11 half uses), KWindowSystem's KX11Extras/NETWM, libplasma and plasmashell are built
# with them, as Debian builds them (none of it runs under Wayland).
step x11 "" $T/build-x11.sh
step x11-session x11 $T/build-x11.sh session
step xkbcommon-x11 x11 bash -c "[ -e $NQ_SYSROOT/usr/lib/libxkbcommon-x11.a ] || $T/build-wayland.sh"
# OpenSSL 3.5.1 (Debian trixie's), static, no assembly, for QtNetwork's TLS (KIO needs QSslSocket).
step openssl fetch bash -c ". $T/env.sh; [ -e \$NQ_SYSROOT/usr/lib/libssl.a ] && exit 0; cd $NQ_SRC/openssl && (make distclean > /dev/null 2>&1; true) && \
	CC=nq-clang AR=\$NQ_LLVM/llvm-ar RANLIB=\$NQ_LLVM/llvm-ranlib ./Configure linux-generic64 no-asm no-shared no-dso \
	no-tests no-apps no-docs no-afalgeng no-async no-secure-memory no-module --prefix=/usr --libdir=lib --openssldir=/etc/ssl && \
	make -j\$NQ_JOBS build_libs && make install_dev DESTDIR=\$NQ_SYSROOT"
# SQLite 3.46.1 (Debian trixie's; Debian's Qt source has no bundled copy): the amalgamation, static.
step sqlite fetch bash -c ". $T/env.sh; [ -e \$NQ_SYSROOT/usr/lib/libsqlite3.a ] && [ -e $NQ_SRC/sqlite/.nq-patched ] && exit 0; cd $NQ_SRC/sqlite && \
	{ [ -e .nq-patched ] || { patch -p1 -s -N < $T/patches/nativeqt-sqlite.diff && touch .nq-patched; }; } && nq-clang -c -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_COLUMN_METADATA \
	-DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_UNLOCK_NOTIFY -DSQLITE_OMIT_LOAD_EXTENSION -o sqlite3.o sqlite3.c && \
	rm -f \$NQ_SYSROOT/usr/lib/libsqlite3.a && \$NQ_LLVM/llvm-ar rcs \$NQ_SYSROOT/usr/lib/libsqlite3.a sqlite3.o && \
	cp sqlite3.h sqlite3ext.h \$NQ_SYSROOT/usr/include/ && \
	printf 'prefix=/usr\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\nName: SQLite\nDescription: SQL database engine\nVersion: 3.46.1\nLibs: -L\${libdir} -lsqlite3\nCflags: -I\${includedir}\n' > \$NQ_SYSROOT/usr/lib/pkgconfig/sqlite3.pc"
step boost-headers fetch bash -c "rm -rf $NQ_SYSROOT/usr/include/boost && cp -a $NQ_SRC/boost/boost $NQ_SYSROOT/usr/include/"
step ecm "fetch" $T/build-kf.sh extra-cmake-modules
step plasma-wayland-protocols "ecm" $T/build-kf.sh plasma-wayland-protocols

# --- Qt again with QtDBus and QtSql, then the modules built on it
# (once: $NQ_BUILD/qtbase/.nq-configured says it was configured this way; delete it to configure again)
step qtbase-dbus "dbus sqlite openssl xkbcommon-x11 host-qtbase-dbus" bash -c "if [ -e $NQ_BUILD/qtbase/.nq-configured ]; then \
	ninja -j\$NQ_JOBS -C $NQ_BUILD/qtbase && ninja -C $NQ_BUILD/qtbase install > /dev/null; else rm -f $NQ_BUILD/qtbase/CMakeCache.txt; \
	NQ_REDO=1 $T/build-qt.sh qtbase && touch $NQ_BUILD/qtbase/.nq-configured; fi"
for m in qtwayland qtshadertools qtsvg qtdeclarative; do
	step $m-2 qtbase-dbus env NQ_REDO=1 $T/build-qt.sh $m
done
step qt5compat "qtdeclarative-2 fetch" $T/build-qt.sh qt5compat
QT="qtbase-dbus qtwayland-2 qtdeclarative-2 qtsvg-2 ecm"

# --- KDE Frameworks 6.13 (tier by tier), then Plasma 6.3.6's libraries, then plasmashell
X="-DWITH_WAYLAND=ON -DBUILD_DESIGNERPLUGIN=OFF -DBUILD_EXAMPLES=OFF"
kf() { local n=$1 deps=$2; shift 2; step $n "runtimes $QT $deps" env NQ_KF_EXTRA="$X $*" $T/build-kf.sh $n; }
kf karchive "zlib" -DWITH_BZIP2=OFF -DWITH_LIBLZMA=OFF -DWITH_LIBZSTD=OFF -DWITH_OPENSSL=OFF
kf kcodecs ""
kf kconfig "host-kconfig"
kf kcoreaddons "" -DENABLE_INOTIFY=OFF
kf kguiaddons "plasma-wayland-protocols"
kf ki18n "" -DBUILD_WITH_QML=ON
kf kitemmodels ""
kf kitemviews ""
kf kwidgetsaddons ""
# (Again once if it was built before the X11 libraries were there.)
[ -e $NQ_SYSROOT/usr/include/KF6/KWindowSystem/KX11Extras ] || rm -f $NQ_BUILD/kf-kwindowsystem/.nq-installed $NQ_BUILD/kf-kwindowsystem/CMakeCache.txt
kf kwindowsystem "plasma-wayland-protocols x11"
kf kdbusaddons ""
kf solid "" -DUDEV_DISABLED=ON -DWITH_NEW_SOLID_JOB=OFF -DWITH_NEW_POWER_ASYNC_API=OFF -DWITH_NEW_POWER_ASYNC_FREEDESKTOP=OFF
kf sonnet "" -DSONNET_USE_QML=ON -DSONNET_NO_BACKENDS=ON
kf kidletime "plasma-wayland-protocols"
kf kglobalaccel ""
kf kcolorscheme "kconfig kguiaddons ki18n"
kf kauth "kcoreaddons" -DKAUTH_BACKEND_NAME=FAKE
kf kcompletion "kconfig kwidgetsaddons kcodecs"
kf kcrash "kcoreaddons kwindowsystem"
kf knotifications "kconfig kcoreaddons" -DUSE_DBUS=ON
kf kpackage "karchive kcoreaddons ki18n"
kf kservice "kconfig kcoreaddons ki18n kdbusaddons"
kf kirigami "qt5compat qtshadertools-2"
kf kstatusnotifieritem "kwindowsystem"
kf kconfigwidgets "kcodecs kconfig kcolorscheme kcoreaddons ki18n kwidgetsaddons"
kf kiconthemes "karchive kconfigwidgets ki18n kcolorscheme kwidgetsaddons" -DKICONTHEMES_USE_QTQUICK=ON -DUSE_BreezeIcons=OFF  # (Breeze's icons come from the disk's /usr/share/icons)
kf kjobwidgets "kcoreaddons kwidgetsaddons knotifications"
kf kxmlgui "kconfigwidgets kglobalaccel kiconthemes kitemviews ki18n kguiaddons" -DFORCE_DISABLE_KGLOBALACCEL=OFF
kf kbookmarks "kconfigwidgets kxmlgui"
kf ksvg "karchive kconfig kcolorscheme kcoreaddons kguiaddons kirigami"
kf kio "karchive kbookmarks kcompletion kconfig kcoreaddons kcrash kdbusaddons kguiaddons ki18n kiconthemes kitemviews kjobwidgets kservice kwidgetsaddons kwindowsystem solid kcolorscheme" \
	-DWITH_WAYLAND=ON -DUSE_FTPD_WSGIDAV_UNITTEST=OFF -DKIO_NO_PUBLIC_QTCONCURRENT=ON
kf qqc2-desktop-style "kirigami kconfig kiconthemes kcolorscheme sonnet kconfigwidgets"
kf kdeclarative "kconfig kglobalaccel kguiaddons ki18n kwidgetsaddons kirigami"
kf kcmutils "kio kitemviews kconfigwidgets kxmlgui kirigami kdeclarative" -DTOOLS_ONLY=OFF
kf kwayland "plasma-wayland-protocols"
kf layer-shell-qt ""
kf plasma-activities "kconfig kcoreaddons boost-headers"
kf plasma-activities-stats "plasma-activities kconfig kcoreaddons boost-headers"
kf libplasma "x11 host-kpackage host-kcmutils kcmutils karchive kconfig kcoreaddons kglobalaccel kguiaddons ki18n kiconthemes kio kwindowsystem knotifications kpackage kirigami ksvg kcolorscheme plasma-activities kwayland plasma-wayland-protocols kdeclarative"
kf plasma5support "libplasma kconfig kcoreaddons kguiaddons ki18n knotifications solid kservice kidletime kio plasma-activities"
kf kirigami-addons "kirigami ki18n kconfig kcoreaddons kguiaddons kglobalaccel"
kf kquickcharts "kirigami qtshadertools-2" -DBUILD_EXAMPLES=OFF
# Plasma's Qt platform theme (KDEPlasmaPlatformTheme6): the colours, fonts and icons of kdeglobals for Qt, and
# their changes (KGlobalSettings notifyChange: a colour scheme applied in System Settings)
kf plasma-integration "x11 kio kstatusnotifieritem kxmlgui kiconthemes knotifications kwindowsystem kguiaddons kconfigwidgets kcolorscheme plasma-wayland-protocols" -DBUILD_QT5=OFF
# What Debian's Plasma has besides: image formats (WebP wallpapers), KNewStuff (Get New...), KHolidays (the
# calendar), libksysguard as a whole (the system monitor applets' sensors and faces)
step qtimageformats "qtbase-dbus fetch" $T/build-qt.sh qtimageformats
kf attica ""
kf ktextwidgets "sonnet kcompletion kconfigwidgets kwidgetsaddons kcolorscheme" -DWITH_TEXT_TO_SPEECH=OFF
kf knewstuff "attica karchive kpackage kirigami kitemmodels kwidgetsaddons ktextwidgets kio"
kf kholidays ""
kf libksysguard "knewstuff kauth kservice kpackage solid kio zlib" -DENABLE_KAUTH_HELPER=OFF
step plasma-shell-patch "fetch" bash -c "python3 $T/plasma-shell-only.py $NQ_SRC/plasma-workspace && \
	mkdir -p $NQ_SRC/plasma-workspace/nativeqt-dbus && \
	cp $NQ_SRC/kscreenlocker/dbus/org.freedesktop.ScreenSaver.xml $NQ_SRC/plasma-workspace/nativeqt-dbus/kf6_org.freedesktop.ScreenSaver.xml && \
	cp $HOME/rvwasm/debian-root-kde/usr/share/dbus-1/interfaces/org.kde.kded6.xml $NQ_SRC/plasma-workspace/nativeqt-dbus/ && \
	(cd $NQ_SRC/kwin/src && $NQ/host/bin/qdbuscpp2xml -A virtualkeyboard_dbus.h -o $NQ_SRC/plasma-workspace/nativeqt-dbus/org.kde.kwin.VirtualKeyboard.xml)"
# plasmashell's static plugins from the other projects' build trees (static-plugins.py), and the QML it runs
# (Debian's packages, from the disk) for Qt's import scanner.
step plasmashell-plugins "libplasma" bash -c ". $T/env.sh; python3 $T/static-plugins.py $NQ_BUILD/nq-plasmashell-plugins.cmake plasmashell \
	$(for m in libplasma kirigami kdeclarative kwindowsystem kcmutils plasma-activities kio kiconthemes ksvg layer-shell-qt kguiaddons kidletime kglobalaccel knotifications kxmlgui solid; do printf "$NQ_BUILD/kf-%s " $m; done)"
# (Qt's scanner takes one directory: a copy of the QML of Debian's Plasma packages and of its QML modules)
Q=$NQ_BUILD/nq-qml-scan
step plasmashell-qml-scan "" bash -c "rm -rf $Q && mkdir -p $Q && R=$HOME/rvwasm/debian-root-kde && \
	cd \$R && find usr/share/plasma usr/lib/riscv64-linux-gnu/qt6/qml/org/kde \( -name '*.qml' -o -name '*.js' -o -name qmldir \) \
	| cpio -pdm --quiet $Q"
kf libkscreen "kwayland plasma-wayland-protocols kconfig kcoreaddons"
kf krunner "kconfig kcoreaddons ki18n kitemmodels kwindowsystem"
# --- The full build (NQ_FULL=1, native/run/full-build.sh): the rest of what Plasma widgets and global themes
# import, in Debian trixie's versions. Each part is tried on its own; one that fails is in the summary and only
# its dependents are skipped. What is not tried is listed at the end of this block, with the reason.
if [ -n "$NQ_FULL" ]; then
	step full-fetch "" $T/fetch-kde.sh qtpositioning qtmultimedia qtsensors libqrencode libdmtx libxml2 libxkbfile \
		bluez-qt prison kimageformats knotifyconfig kunitconversion milou kactivitymanagerd kscreen kdeplasma-addons
	# Qt modules Debian ships: Positioning (night light's location), Multimedia (KNotifyConfig's sound preview,
	# Prison's scanner; no media backend here), Sensors (KScreen)
	step qtpositioning "qtbase-dbus full-fetch" $T/build-qt.sh qtpositioning
	step qtmultimedia "qtdeclarative-2 qtshadertools-2 full-fetch" env NQ_QT_EXTRA="-DFEATURE_ffmpeg=OFF -DFEATURE_gstreamer=OFF -DFEATURE_pulseaudio=OFF -DFEATURE_alsa=OFF" $T/build-qt.sh qtmultimedia
	step qtsensors "qtdeclarative-2 full-fetch" $T/build-qt.sh qtsensors
	# C libraries: Prison's QR and Data Matrix codes; the keyboard KCM's xkbfile and xkbregistry (libxml2)
	step libqrencode full-fetch $T/build-clib.sh libqrencode cmake -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITHOUT_PNG=ON
	step libdmtx full-fetch env NQ_CLIB_INSTALL='cp $B/libdmtx.a $NQ_SYSROOT/usr/lib/ && cp $S/dmtx.h $NQ_SYSROOT/usr/include/' $T/build-clib.sh libdmtx cmake -DDMTX_SHARED=OFF
	step libxml2 full-fetch $T/build-clib.sh libxml2 cmake -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF \
		-DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_ICU=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_ZLIB=OFF
	step libxkbfile "x11 full-fetch" $T/build-clib.sh libxkbfile meson
	step xkbregistry "libxml2" env NQ_XKBREGISTRY=1 $T/build-wayland.sh
	# KDE Frameworks 6.13
	kf knotifyconfig "knotifications kio qtmultimedia"
	kf kimageformats "karchive" -DKIMAGEFORMATS_HEIF=OFF -DKIMAGEFORMATS_JXR=OFF -DKIMAGEFORMATS_DDS=ON
	kf prison "qtmultimedia libqrencode libdmtx" -DWITH_QUICK=ON
	kf bluez-qt ""
	kf kunitconversion "ki18n"
	# Plasma 6.3 (Debian: milou 6.3.5, kactivitymanagerd 6.3.4; kscreen and kdeplasma-addons are not on its disk)
	kf milou "krunner libplasma"
	kf kactivitymanagerd "kio kcrash kdbusaddons boost-headers"
	kf kscreen "libkscreen qtsensors libplasma layer-shell-qt"
	kf kdeplasma-addons "libplasma plasma5support kunitconversion knewstuff kholidays kcmutils sonnet kxmlgui krunner kdeclarative" \
		-DCMAKE_DISABLE_FIND_PACKAGE_ICU=ON -DCMAKE_DISABLE_FIND_PACKAGE_Qt6WebEngineQuick=ON -DCMAKE_DISABLE_FIND_PACKAGE_Qt6Quick3D=ON
	# (and plasma-workspace and plasma-desktop-parts again below, with what they find now: Klipper with Prison,
	# libcolorcorrect with Qt Positioning, the notifications engine with KNotifyConfig, the keyboard KCM's parts)
	rm -f $NQ_BUILD/kf-plasma-workspace/.nq-installed $NQ_BUILD/kf-plasma-desktop-parts/.nq-installed
	# Not tried: plasma-pa (libpulse with its GLib main loop, GIO, libcanberra: GLib needs libffi, and there is no
	# PulseAudio here; Debian's disk has no plasma-pa either), plasma-nm (libnm, GLib, Qt WebEngine; not on Debian's
	# disk), KPipeWire (libpipewire loads SPA plugins at run time, FFmpeg, GBM; a QML stand-in is linked instead),
	# powerdevil (udev, DDC; not on Debian's disk).
	for x in plasma-pa plasma-nm kpipewire powerdevil; do printf "%-28s %-8s %7s  %s\n" $x "not tried" - "see run/overnight.sh" >> $S; done
fi
kf plasma-workspace "plasma5support libksysguard knewstuff ktextwidgets kholidays libkscreen krunner plasma-activities-stats plasmashell-plugins plasmashell-qml-scan x11 x11-session plasma-shell-patch libplasma kcrash kdbusaddons kstatusnotifieritem kxmlgui layer-shell-qt solid kio plasma-activities kwayland kglobalaccel knotifications kpackage kservice ksvg" \
	-DINSTALL_SDDM_WAYLAND_SESSION=OFF -DPLASMA_X11_DEFAULT_SESSION=OFF \
	-DNQ_STATIC_PLUGINS=$NQ_BUILD/nq-plasmashell-plugins.cmake -DNQ_QML_SCAN=$Q -DNQ_CMAKE_DIR=$T  # (with X11 as Debian: its WITH_X11=OFF does not compile in 6.3.6)

# plasma-desktop's folder view and activity switcher (plasma-desktop-parts/), then plasmashell linked again
# with them.
step plasma-desktop-src fetch bash -c "$T/fetch-kde.sh plasma-desktop && rm -rf $NQ_SRC/plasma-desktop-parts && \
	cp -r $T/plasma-desktop-parts $NQ_SRC/plasma-desktop-parts && mkdir -p $NQ_SRC/plasma-desktop-parts/kcms && \
	cp -r $NQ_SRC/plasma-desktop/kcms/recentFiles $NQ_SRC/plasma-desktop-parts/kcms/ && $T/fetch-kde.sh libksysguard"
kf plasma-desktop-parts "plasma-desktop-src plasma-workspace libksysguard kio knotifications kxmlgui kauth kservice kitemmodels plasma-activities-stats" \
	-DPLASMA_DESKTOP_SRC=$NQ_SRC/plasma-desktop -DPLASMA_WORKSPACE_SRC=$NQ_SRC/plasma-workspace -DLIBKSYSGUARD_SRC=$NQ_SRC/libksysguard -DNQ_CMAKE_DIR=$T
step plasmashell-plugins-2 "plasma-desktop-parts kirigami-addons kquickcharts plasma-integration qtimageformats" bash -c ". $T/env.sh; python3 $T/static-plugins.py $NQ_BUILD/nq-plasmashell-plugins.cmake plasmashell \
	$(ls -d $NQ_BUILD/kf-* | grep -v 'kf-plasma-workspace\|kf-extra-cmake-modules\|kf-kactivitymanagerd' | tr '\n' ' ') \
	archives:$NQ_BUILD/kf-plasma-workspace $(for l in KF6Bookmarks KF6KIOFileWidgets KF6KIOWidgets KF6Solid KF6Completion KF6JobWidgets KF6ItemViews KF6XmlGui KF6ConfigWidgets KF6IconThemes KF6WidgetsAddons KF6Service; do printf "lib:%s " $l; done)"
step plasmashell-relink "plasmashell-plugins-2" bash -c ". $T/env.sh; ninja -j\$NQ_JOBS -C $NQ_BUILD/kf-plasma-workspace bin/plasmashell && cp $NQ_BUILD/kf-plasma-workspace/bin/plasmashell $NQ_SYSROOT/usr/bin/plasmashell"
echo "# finished $(date '+%F %T'): $(grep -c ' ok ' $S) ok, $(grep -c ' FAILED ' $S) failed, $(grep -c ' skipped ' $S) skipped" >> $S
