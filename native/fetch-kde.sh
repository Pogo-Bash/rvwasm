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
	# the compression libraries KArchive needs for .tar.xz / .tar.bz2 / .tar.zst (shared builds: shared/build-kf-shared.sh xz bzip2 zstd; sums below)
	xz) echo https://github.com/tukaani-project/xz/releases/download/v5.6.4/xz-5.6.4.tar.xz ;;
	zstd) echo https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz ;;
	bzip2) echo https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz ;;
	qcoro) echo https://github.com/qcoro/qcoro/archive/refs/tags/v0.11.0.tar.gz ;;
	*) echo https://download.kde.org/stable/plasma/$PL/$1-$PL.tar.xz ;;
	esac
}
# sha256 of the tarballs that have one recorded here (checked after a download and again when the file is already there)
sha() {
	case $1 in
	xz-5.6.4.tar.xz) echo 829ccfe79d769748f7557e7a4429a64d06858e27e1e362e25d01ab7b931d9c95 ;;
	zstd-1.5.6.tar.gz) echo 8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1 ;;
	bzip2-1.0.8.tar.gz) echo ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269 ;;
	esac
}
mkdir -p $NQ_SRC/dl
for n in ${*:-dbus $kf $pl}; do
	[ -d $NQ_SRC/$n ] && continue
	u=$(url $n)
	f=$NQ_SRC/dl/$(basename $u)
	[ -s $f ] || curl -sfL -o $f $u || { echo "fetch-kde: $n: $u not found"; continue; }
	want=$(sha $(basename $u))
	if [ -n "$want" ] && [ "$(sha256sum $f | cut -d' ' -f1)" != "$want" ]; then
		echo "fetch-kde: $n: $f does not match its recorded sha256; removed" >&2
		rm -f $f
		continue
	fi
	mkdir -p $NQ_SRC/$n && tar xf $f -C $NQ_SRC/$n --strip-components=1
	echo "fetch-kde: $n"
done
