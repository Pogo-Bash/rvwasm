#!/bin/bash
# Shared versions of the C libraries that more than one Qt library or plugin would otherwise each carry a copy of
# (libwayland-client: the shell-integration plugins; libxkbcommon: Gui, WaylandClient, the compose input context).
# No rebuild: the static archives (built -fPIC for the static build) linked --whole-archive into one side module
# each; only default-visibility (WL_EXPORT, XKB_EXPORT) symbols are exported. The .a is then removed from
# sysroot-shared so the Qt build links the .so (prep-sysroot.sh leaves it out too).
# Libraries that only one library uses (FreeType, fontconfig, expat, libdbus, OpenSSL, harfbuzz, libpng, libjpeg,
# PCRE2, double-conversion) stay static inside it.
set -e
. "$(dirname "$0")/env.sh"
L=$NQ_SYSROOT/usr/lib
for spec in wayland-client:0 xkbcommon:0; do
	n=${spec%:*}; v=${spec#*:}
	[ -e $L/lib$n.a ] || { echo "lib$n.a already replaced"; continue; }
	$NQS_TREE/bin/nqs-clang -shared -o $L/lib$n.so.$v -Wl,--whole-archive $L/lib$n.a -Wl,--no-whole-archive
	ln -sf lib$n.so.$v $L/lib$n.so
	mkdir -p $NQ_SYSROOT/usr/lib/nqshared-removed && mv $L/lib$n.a $NQ_SYSROOT/usr/lib/nqshared-removed/
	$NQS_TREE/check-dylib.py $L/lib$n.so.$v --provider ${NQS_LD_WASM:-$NQ/ref/ld-wasm}
done
