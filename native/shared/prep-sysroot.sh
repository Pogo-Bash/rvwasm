#!/bin/bash
# $NQ/sysroot-shared: a copy of the static build's sysroot (musl, kernel headers, libc++, the C libraries the
# static build installed) without any Qt/KF6/Plasma of ours, so that the shared Qt is installed into a clean
# prefix. Never writes to $NQ/sysroot. Safe to run again.
set -e
. "$(dirname "$0")/env.sh"
S=$NQ/sysroot
mkdir -p $NQ_SYSROOT
rsync -a --exclude=/usr/bin --exclude=/usr/share --exclude=/usr/lib/libexec --exclude=/usr/lib/qt6 \
	--exclude=/usr/lib/kconf_update_bin --exclude='/usr/lib/libQt6*' --exclude='/usr/lib/libKF6*' \
	--exclude='/usr/lib/libK*' --exclude='/usr/lib/libPlasma*' --exclude='/usr/lib/cmake/Qt6*' --exclude='/usr/lib/cmake/KF6*' --exclude='/usr/lib/cmake/K[A-Z]*' --exclude='/usr/lib/cmake/Plasma*' --exclude='/usr/lib/cmake/Lib*' --exclude=/usr/lib/cmake/LayerShellQt --exclude=/usr/include/qt6 \
	--exclude=/usr/include/KF6 --exclude=/usr/lib/libgallium.a --exclude=/usr/lib/libwayland-client.a --exclude=/usr/lib/libxkbcommon.a $S/ $NQ_SYSROOT/
# This musl's dlfcn.h redirects dlsym to __dlsym_time64 (_REDIR_TIME64 is 1 in bits/alltypes.h), but neither its
# libc.a nor ld-wasm defines that name (they have dlsym and __dlsym_redir_time64): QLibrary's dlsym calls would be
# unresolved imports. No time_t is involved in dlsym; call dlsym by its own name.
sed -i 's/^#if _REDIR_TIME64$/#if 0 \&\& _REDIR_TIME64   \/* nqshared: see shared\/prep-sysroot.sh *\//' $NQ_SYSROOT/usr/include/dlfcn.h
grep -n -B1 -A2 "nqshared" $NQ_SYSROOT/usr/include/dlfcn.h
