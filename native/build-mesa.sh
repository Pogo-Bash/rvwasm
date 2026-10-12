#!/bin/bash
# Mesa 25.0.7 (Debian trixie's) for native linux-wasm programs: EGL (Wayland and surfaceless platforms) and
# GLES 2/3 + desktop GL entry points, with one Gallium driver: virgl, whose vtest winsys ("virpipe") talks to
# the page's GPU server over $VTEST_SOCKET_NAME (gpu/PROTOCOL.md), and softpipe: without a software driver Mesa
# builds no swrast screen, and virpipe is one (GALLIUM_DRIVER=virpipe picks it in the swrast loader). Static (patches/nativeqt-mesa.diff makes
# Mesa's shared libraries static ones), no LLVM. Also libdrm 2.4.124 and wayland-protocols 1.45 (headers and
# XML; as Debian trixie). Options otherwise as debian/rules (src/mesa-debian) for an arch without LLVM.
set -e
. "$(dirname "$0")/env.sh"
. $NQ_TREE/meson-cross.sh
export NQ_LAUNCHER=ccache
export NQ_CFLAGS_LAST=-Wno-error=format
export PATH=$NQ/host/bin:$PATH
cd $NQ_SRC
[ -d libdrm ] || { curl -sfL https://dri.freedesktop.org/libdrm/libdrm-2.4.124.tar.xz | tar xJ && mv libdrm-2.4.124 libdrm; }
[ -d wayland-protocols ] || { curl -sfL https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/1.45/downloads/wayland-protocols-1.45.tar.xz | tar xJ && mv wayland-protocols-1.45 wayland-protocols; }

if [ ! -e $NQ_BUILD/wayland-protocols/.nq-installed ]; then
	meson setup --wipe $NQ_BUILD/wayland-protocols $NQ_SRC/wayland-protocols --cross-file $NQ_CROSS --prefix=/usr \
		--datadir=share -Dtests=false
	DESTDIR=$NQ_SYSROOT ninja -j$NQ_JOBS -C $NQ_BUILD/wayland-protocols install > /dev/null
	touch $NQ_BUILD/wayland-protocols/.nq-installed
fi
if [ ! -e $NQ_BUILD/libdrm/.nq-installed ]; then
	meson setup --wipe $NQ_BUILD/libdrm $NQ_SRC/libdrm --cross-file $NQ_CROSS --prefix=/usr --libdir=lib \
		-Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
		-Domap=disabled -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled -Dvc4=disabled \
		-Detnaviv=disabled -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled -Dtests=false \
		-Dinstall-test-programs=false -Dudev=false
	ninja -C $NQ_BUILD/libdrm -j$NQ_JOBS
	DESTDIR=$NQ_SYSROOT ninja -j$NQ_JOBS -C $NQ_BUILD/libdrm install > /dev/null
	touch $NQ_BUILD/libdrm/.nq-installed
fi

S=$NQ_SRC/mesa
if [ ! -e $S/.nq-patched ]; then
	patch -d $S -p1 -s -N < $NQ_TREE/patches/nativeqt-mesa.diff
	touch $S/.nq-patched
fi
B=$NQ_BUILD/mesa
t0=$(date +%s)
if [ ! -e $B/build.ninja ] || [ -n "$NQ_REDO" ]; then
	meson setup $( [ -e $B/build.ninja ] && echo --reconfigure ) $B $S --cross-file $NQ_CROSS --prefix=/usr --libdir=lib \
		-Dbuild.pkg_config_path=$NQ/host/lib/aarch64-linux-gnu/pkgconfig \
		-Dplatforms=wayland -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dglx=disabled \
		-Dgbm=disabled -Dglvnd=disabled -Dshared-glapi=enabled \
		-Dgallium-drivers=softpipe,virgl -Dvulkan-drivers= -Dvulkan-layers= -Dllvm=disabled \
		-Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-nine=false \
		-Dgallium-opencl=disabled -Dgallium-rusticl=false -Dteflon=false -Dosmesa=false \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled -Dzlib=disabled \
		-Dshader-cache=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dsse2=false -Dtools= \
		-Dbuild-tests=false -Db_ndebug=true -Dbuildtype=release
fi
ninja -C $B -j$NQ_JOBS
DESTDIR=$NQ_SYSROOT ninja -j$NQ_JOBS -C $B install > /dev/null
# libgallium is a static archive (with the internal libraries it links bundled by Meson) that Meson still
# names as the shared library: give it an archive's name.
mv -f $NQ_SYSROOT/usr/lib/libgallium-25.0.7.so $NQ_SYSROOT/usr/lib/libgallium.a
sed -i 's/-lgallium-25.0.7/-lgallium/' $NQ_SYSROOT/usr/lib/pkgconfig/glesv2.pc
# What -lEGL and -lGLESv2 need after them, for links that name only the library (bin/nq-clang reads these).
mkdir -p $NQ_SYSROOT/usr/lib/nq-deps
echo "-lGLESv2 -lgallium -ldrm -lwayland-client -lwayland-server -lc++" > $NQ_SYSROOT/usr/lib/nq-deps/EGL
echo "-lgallium -ldrm -lc++" > $NQ_SYSROOT/usr/lib/nq-deps/GLESv2
echo "mesa: $(( $(date +%s) - t0 )) s"
