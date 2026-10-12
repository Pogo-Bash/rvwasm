#!/bin/bash
# Mesa 25.0.7 (as Debian trixie's mesa 25.0.7-2+deb13u1) and Debian's packaging, into $NQ/src.
set -e
. "$(dirname "$0")/env.sh"
cd $NQ_SRC
[ -d mesa ] || { curl -sfL https://archive.mesa3d.org/mesa-25.0.7.tar.xz | tar xJ && mv mesa-25.0.7 mesa; }
[ -d mesa-debian ] || { mkdir mesa-debian && curl -sfL http://deb.debian.org/debian/pool/main/m/mesa/mesa_25.0.7-2+deb13u1.debian.tar.xz | tar xJ -C mesa-debian; }
echo mesa fetched
