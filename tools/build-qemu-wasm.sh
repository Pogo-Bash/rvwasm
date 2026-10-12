#!/bin/bash
# Build qemu-wasm (QEMU compiled to WebAssembly, https://github.com/ktock/qemu-wasm)
# for riscv64 full-system emulation, and package it with our guest
# (tools/build-qemu-guest.sh) for the browser. This is the system rv is
# compared against. Run inside the build VM; needs Docker.
#
#   build-qemu-wasm.sh env       the Emscripten build environment (once, slow)
#   build-qemu-wasm.sh qemu      configure and build qemu-system-riscv64
#   build-qemu-wasm.sh package   pack kernel + root filesystem, lay out ~/rvwasm/www/qemu
#
# Follows the project's README. Two deviations, both forced:
#   - emsdk 3.1.58 instead of the pinned 3.1.50, which has no arm64 image;
#   - zlib from its GitHub release: zlib.net no longer serves 1.3.1 at the pinned URL.
#
# VARIANT=nodebug builds a second copy without the README's -g, in /build-nodebug
# in the container, and packages it as ~/rvwasm/www/qemu-nodebug. With -g
# Emscripten holds back Binaryen's optimiser to keep the debug information
# valid; without it the result is the best case for qemu-wasm.

set -e

S="$(realpath "$(dirname "$0")/..")"
W="$HOME/rvwasm"
Q="$W/qemu-wasm"
NAME=build-qemu-wasm
EMSDK="${EMSDK:-3.1.58-arm64}"
VARIANT="${VARIANT:-}"
if [ "$VARIANT" = nodebug ]; then
	B=/build-nodebug
	G=
	OUT="$W/www/qemu-nodebug"
else
	B=/build
	G=-g
	OUT="$W/www/qemu"
fi

case "$1" in
env)
	[ -d "$Q" ] || git clone -q --depth 1 https://github.com/ktock/qemu-wasm "$Q"
	sed -i 's|https://zlib.net/zlib-$ZLIB_VERSION.tar.xz|https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.xz|' "$Q/Dockerfile"
	sudo docker build --build-arg EMSDK_VERSION_QEMU="$EMSDK" -t buildqemu - < "$Q/Dockerfile"
	;;
qemu)
	# (The container is kept if it is already running: it holds the other variant's build.)
	sudo docker inspect "$NAME" > /dev/null 2>&1 ||
		sudo docker run --rm -d --name "$NAME" --cpus "${CPUS:-6}" -v "$Q":/qemu/:ro buildqemu
	sudo docker exec "$NAME" mkdir -p "$B"
	# The flags from the project's README for a riscv64 guest.
	EXTRA_CFLAGS="-O3 $G -Wno-error=unused-command-line-argument -matomics -mbulk-memory -DNDEBUG -DG_DISABLE_ASSERT -D_GNU_SOURCE -sASYNCIFY=1 -pthread -sPROXY_TO_PTHREAD=1 -sFORCE_FILESYSTEM -sALLOW_TABLE_GROWTH -sTOTAL_MEMORY=2300MB -sWASM_BIGINT -sMALLOC=mimalloc --js-library=/build/node_modules/xterm-pty/emscripten-pty.js -sEXPORT_ES6=1 -sASYNCIFY_IMPORTS=ffi_call_js"
	sudo docker exec -w "$B" "$NAME" emconfigure /qemu/configure --static --target-list=riscv64-softmmu --cpu=wasm32 --cross-prefix= \
		--without-default-features --enable-system --with-coroutine=fiber --enable-virtfs \
		--extra-cflags="$EXTRA_CFLAGS" --extra-cxxflags="$EXTRA_CFLAGS" \
		--extra-ldflags="-sEXPORTED_RUNTIME_METHODS=getTempRet0,setTempRet0,addFunction,removeFunction,TTY,FS"
	sudo docker exec -w "$B" "$NAME" sh -c 'emmake make -j ${CPUS:-6} qemu-system-riscv64'
	;;
package)
	PACK="$W/qemu-guest/pack"
	rm -rf "$PACK" && mkdir -p "$PACK" "$OUT/vendor"
	cp "$W/qemu-guest/Image" "$W/qemu-guest/rootfs.bin" "$Q/pc-bios/opensbi-riscv64-generic-fw_dynamic.bin" "$PACK/"
	sudo docker exec "$NAME" rm -rf /pack
	sudo docker cp "$PACK" "$NAME":/pack
	sudo docker exec -w "$B" "$NAME" /bin/sh -c "/emsdk/upstream/emscripten/tools/file_packager.py qemu-system-riscv64.data --preload /pack > load.js"
	sudo docker cp "$NAME":$B/qemu-system-riscv64 "$OUT/out.js"
	for f in qemu-system-riscv64.wasm qemu-system-riscv64.worker.js qemu-system-riscv64.data load.js; do
		sudo docker cp "$NAME":$B/$f "$OUT/$f"
	done
	sudo chown -R "$(id -u):$(id -g)" "$OUT"
	# The terminal libraries the example page takes from a CDN, served locally.
	[ -f "$OUT/vendor/xterm.js" ] || curl -fsSL -o "$OUT/vendor/xterm.js" https://unpkg.com/xterm@5.3.0/lib/xterm.js
	[ -f "$OUT/vendor/xterm.css" ] || curl -fsSL -o "$OUT/vendor/xterm.css" https://unpkg.com/xterm@5.3.0/css/xterm.css
	[ -f "$OUT/vendor/xterm-pty.js" ] || curl -fsSL -o "$OUT/vendor/xterm-pty.js" https://unpkg.com/xterm-pty/index.js
	cp "$S/tools/www/qemu.html" "$OUT/index.html"
	ls -la "$OUT"
	;;
*)
	echo "usage: $0 env|qemu|package" >&2
	exit 2
	;;
esac
