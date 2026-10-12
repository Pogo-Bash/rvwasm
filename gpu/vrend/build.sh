#!/bin/bash
# Build virglrenderer's vrend for the page (WebAssembly, WebGL2) in the build VM. The sources of this
# directory arrive in $SRC (gpu/vrend of the gpu-host worktree, sent over stdin by build/vrend-sync.sh);
# virglrenderer is a checkout in $VIRGL (1.1.1). Output in $OUT. Emscripten is the one in $EMSDK; the
# builds are under $GPUHOST (the build VM's ~/emsdk and ~/gpuhost by default; docker/ sets its own).
set -eu
EMSDK=${EMSDK:-$HOME/emsdk}
GPUHOST=${GPUHOST:-$HOME/gpuhost}
source $EMSDK/emsdk_env.sh > /dev/null 2>&1
SRC=${SRC:-$GPUHOST/vrend-src}
VIRGL=${VIRGL:-$GPUHOST/virglrenderer}
PREFIX=$GPUHOST/prefix
OUT=${OUT:-$GPUHOST/out}
JOBS=${JOBS:-6}
mkdir -p $PREFIX/include $PREFIX/lib/pkgconfig $OUT

python3 $SRC/gen-gl-extra.py $EMSDK/upstream/emscripten/cache/sysroot/include $SRC/epoxy/include/epoxy/gl_extra.h
# The epoxy stand-in: headers, a static library, a .pc file meson finds.
rm -rf $PREFIX/include/epoxy && cp -r $SRC/epoxy/include/epoxy $PREFIX/include/
emcc -O2 -I$PREFIX/include -c $SRC/epoxy/epoxy.c -o $PREFIX/epoxy.o
emcc -O2 -I$PREFIX/include -c $SRC/epoxy/missing.c -o $PREFIX/missing.o
emar rcs $PREFIX/lib/libepoxy.a $PREFIX/epoxy.o $PREFIX/missing.o
sed "s|@PREFIX@|$PREFIX|" $SRC/epoxy.pc.in > $PREFIX/lib/pkgconfig/epoxy.pc

# virglrenderer 1.1.1 with our patch (virglrenderer-wasm.diff: Emscripten taken for Linux, no kcmp).
if [ ! -d $VIRGL ]; then
	git clone -q --depth 1 --branch 1.1.1 https://gitlab.freedesktop.org/virgl/virglrenderer.git $VIRGL
fi
git -C $VIRGL checkout -q -- .
git -C $VIRGL apply $SRC/virglrenderer-wasm.diff

# virglrenderer as a static library: no EGL, no GLX, no DRM, no venus, no video, no tests.
cd $VIRGL
if [ ! -d build-wasm ]; then
	PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig \
	meson setup build-wasm --cross-file $SRC/cross-emscripten.txt -Ddefault_library=static \
		-Dplatforms= -Dminigbm_allocation=false -Dvenus=false -Dvideo=false -Dtests=false \
		-Dcheck-gl-errors=false -Ddrm-renderers=[] -Drender-server=false -Dbuildtype=release
fi
ninja -C build-wasm -j$JOBS src/libvirglrenderer.a

# The module: rvgpu.c (the entry points gpu-exec.js calls) linked with virglrenderer, the epoxy stand-in
# and Emscripten's GL (WebGL2; FULL_ES3 emulates the mapped buffers and client arrays WebGL lacks). A stack of
# 4 MiB: Emscripten's default is 64 KiB, and vrend's shader translator keeps its whole state (struct dump_ctx) on
# the stack; overflowing it went over the module's data unseen (a vertex shader asked for interface blocks
# as if a geometry shader were there, then the heap broke).
B=$VIRGL/build-wasm
emcc -O2 -I$VIRGL/src -I$B -I$B/src -I$PREFIX/include -c $SRC/rvgpu.c -o $B/rvgpu.o
emcc -O2 $B/rvgpu.o $B/src/libvirglrenderer.a \
	$PREFIX/lib/libepoxy.a -o $OUT/vrend.js --profiling-funcs \
	-sMODULARIZE=1 -sEXPORT_NAME=VrendModule -sENVIRONMENT=web,worker \
	-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1 -sALLOW_MEMORY_GROWTH=1 \
	-sSTACK_SIZE=4194304 -sSTACK_OVERFLOW_CHECK=1 \
	-sEXPORTED_RUNTIME_METHODS=GL,HEAPU8,HEAPU32 -sEXPORTED_FUNCTIONS=_malloc,_free \
	-sERROR_ON_UNDEFINED_SYMBOLS=1 -Wl,--error-limit=0 2> $OUT/link.log || {
	# GL functions WebGL2 lacks: stubs for them (gen-missing.py into missing.c), then once more.
	grep -oE "undefined symbol: gl[A-Za-z0-9_]+" $OUT/link.log | sed "s/undefined symbol: //" > $OUT/missing.txt
	if [ -s $OUT/missing.txt ] && [ -z "${AGAIN:-}" ]; then
		python3 $SRC/gen-missing.py $EMSDK/upstream/emscripten/cache/sysroot/include $OUT/missing.txt $SRC/epoxy/missing.c $PREFIX/include/epoxy/gl_extra.h
		cp $SRC/epoxy/missing.c $OUT/missing.c
		AGAIN=1 exec bash $0
	fi
	grep -E "undefined symbol|error" $OUT/link.log | grep -v "undefined symbol: gl" | head -40; exit 1; }
ls -la $OUT
