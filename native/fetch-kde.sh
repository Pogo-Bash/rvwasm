#!/bin/bash
# Sources for stage e (the versions of Debian trixie): D-Bus, extra-cmake-modules and KDE Frameworks 6.13,
# Plasma 6.3.6, into $NQ/src/NAME (tarballs kept in $NQ/src/dl). Names as arguments, or all of them.
set -e
. "$(dirname "$0")/env.sh"
KF=6.13.0 PL=6.3.6
kf="extra-cmake-modules karchive kconfig kcoreaddons ki18n kwidgetsaddons kwindowsystem kguiaddons kitemmodels
	kdbusaddons kcodecs kcolorscheme kglobalaccel knotifications kcrash kauth kconfigwidgets kiconthemes
	kxmlgui kio kservice kpackage ksvg kirigami kjobwidgets kcompletion kbookmarks solid kstatusnotifieritem
	kdeclarative kcmutils kquickcharts kidletime kitemviews ktextwidgets sonnet knotifyconfig kunitconversion krunner
	kwallet kparts kholidays kfilemetadata baloo knewstuff kded attica breeze-icons qqc2-desktop-style
	bluez-qt networkmanager-qt prison kimageformats purpose"
pl="libplasma plasma-activities plasma-activities-stats layer-shell-qt kwayland plasma-workspace
	plasma-wayland-protocols kscreenlocker libkscreen breeze plasma-desktop libksysguard plasma5support kirigami-addons plasma-integration
	kdeplasma-addons milou plasma-pa plasma-nm kscreen powerdevil kactivitymanagerd kpipewire"
url() {
	case " $(echo $kf) " in *" $1 "*) echo https://download.kde.org/stable/frameworks/${KF%.*}/$1-$KF.tar.xz; return ;; esac
	case $1 in
	plasma-wayland-protocols) echo https://download.kde.org/stable/plasma-wayland-protocols/plasma-wayland-protocols-1.16.0.tar.xz ;;
	dbus) echo https://dbus.freedesktop.org/releases/dbus/dbus-1.16.2.tar.xz ;;
	zlib) echo https://zlib.net/fossils/zlib-1.3.1.tar.gz ;;
	openssl) echo https://github.com/openssl/openssl/releases/download/openssl-3.5.1/openssl-3.5.1.tar.gz ;;
	sqlite) echo https://www.sqlite.org/2024/sqlite-autoconf-3460100.tar.gz ;;
	boost) echo https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2 ;;
	qt5compat) echo https://download.qt.io/archive/qt/6.8/6.8.2/submodules/qt5compat-everywhere-src-6.8.2.tar.xz ;;
	qtimageformats) echo https://download.qt.io/archive/qt/6.8/6.8.2/submodules/qtimageformats-everywhere-src-6.8.2.tar.xz ;;
	qtmultimedia|qtpositioning|qtsensors) echo https://download.qt.io/archive/qt/6.8/6.8.2/submodules/$1-everywhere-src-6.8.2.tar.xz ;;
	libqrencode) echo https://github.com/fukuchi/libqrencode/archive/refs/tags/v4.1.1.tar.gz ;;
	libdmtx) echo https://github.com/dmtx/libdmtx/archive/refs/tags/v0.7.7.tar.gz ;;
	libxml2) echo https://download.gnome.org/sources/libxml2/2.12/libxml2-2.12.7.tar.xz ;;
	libxkbfile) echo https://www.x.org/releases/individual/lib/libxkbfile-1.1.3.tar.xz ;;
	kirigami-addons) echo https://download.kde.org/stable/kirigami-addons/kirigami-addons-1.7.0.tar.xz ;;
	qcoro) echo https://github.com/qcoro/qcoro/archive/refs/tags/v0.11.0.tar.gz ;;
	*) echo https://download.kde.org/stable/plasma/$PL/$1-$PL.tar.xz ;;
	esac
}
mkdir -p $NQ_SRC/dl
for n in ${*:-dbus $kf $pl}; do
	[ -d $NQ_SRC/$n ] && continue
	u=$(url $n)
	f=$NQ_SRC/dl/$(basename $u)
	[ -s $f ] || curl -sfL -o $f $u || { echo "fetch-kde: $n: $u not found"; continue; }
	mkdir -p $NQ_SRC/$n && tar xf $f -C $NQ_SRC/$n --strip-components=1
	echo "fetch-kde: $n"
done
