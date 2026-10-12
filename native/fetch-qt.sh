#!/bin/bash
# Qt 6.8.2 as Debian trixie ships it: orig tarballs and debian/ (for configure options), from deb.debian.org.
set -e
. "$(dirname "$0")/env.sh"
mkdir -p $NQ/dl $NQ_SRC
cd $NQ/dl
D=https://deb.debian.org/debian/pool/main/q
for f in qt6-base/qt6-base_6.8.2+dfsg.orig.tar.xz qt6-base/qt6-base_6.8.2+dfsg-9+deb13u2.debian.tar.xz \
	qt6-wayland/qt6-wayland_6.8.2.orig.tar.xz qt6-wayland/qt6-wayland_6.8.2-4.debian.tar.xz \
	qt6-declarative/qt6-declarative_6.8.2+dfsg.orig.tar.xz qt6-declarative/qt6-declarative_6.8.2+dfsg-7.debian.tar.xz \
	qt6-shadertools/qt6-shadertools_6.8.2.orig.tar.xz qt6-shadertools/qt6-shadertools_6.8.2-3.debian.tar.xz \
	qt6-svg/qt6-svg_6.8.2.orig.tar.xz qt6-svg/qt6-svg_6.8.2-3.debian.tar.xz; do
	[ -e "$(basename $f)" ] || curl -sSfLO "$D/$f"
done
for m in base wayland declarative shadertools svg; do
	[ -d $NQ_SRC/qt$m ] && continue
	mkdir -p $NQ_SRC/qt$m
	tar -xf qt6-${m}_6.8.2*.orig.tar.xz -C $NQ_SRC/qt$m --strip-components=1
	tar -xf qt6-${m}_6.8.2*.debian.tar.xz -C $NQ_SRC/qt$m
done
ls $NQ_SRC
