#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/build/native.sh [ninja targets])
# The native Mesa build of the test bed, from this build's tree: wgvk as a Vulkan ICD on Dawn native,
# zink and llvmpipe (GL through EGL surfaceless), lavapipe (the reference), mesa_clc + vtn_bindgen2
# (installed into $B/native-tools for the Emscripten build, which cannot run them). Incremental.
# GPU_NATIVE=full (the default for the integration build `webgpu`) builds all of that; GPU_NATIVE=min (the
# default for track builds: the VM's disk is short) builds wgvk only (+ zink with GPU_NATIVE=zink), and the
# tests then take lavapipe from the integration build ($W/b/webgpu/native). Changing it needs a fresh $B/native.
# Output: $B/native; the ICD json: $B/native/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json; Mesa's own devenv
# (meson devenv -C $B/native) points VK_DRIVER_FILES, LIBGL_DRIVERS_PATH etc. at the build.
set -e
. "$(dirname "$0")/env.sh"
bash "$(dirname "$0")/tree.sh"
bash "$(dirname "$0")/vkr-tree.sh"	# (T5: vkr's sources, src/vkr-src)
bash "$(dirname "$0")/dawn-pc.sh"
O=$B/native
GPU_NATIVE=${GPU_NATIVE:-$([ $NAME = webgpu ] && echo full || echo min)}
# Window systems (T7): wgvk's WSI offers VK_KHR_xcb_surface/xlib_surface and wayland_surface natively (zink's
# kopper on X11 needs them: Blender's UI, glxinfo under Xvfb). GPU_PLATFORMS= (empty) builds none, as before.
# A build made with other platforms is reconfigured (meson configure), not remade.
PLATFORMS=${GPU_PLATFORMS-x11,wayland}
case $GPU_NATIVE in
full)	PROFILE=(-Dplatforms=$PLATFORMS -Dglx=disabled -Degl=enabled -Dgbm=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled
		-Dgallium-drivers=zink,llvmpipe -Dvulkan-drivers=webgpu,swrast
		-Dllvm=enabled -Dshared-llvm=enabled -Dmesa-clc=enabled -Dinstall-mesa-clc=true
		-Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true) ;;
zink)	PROFILE=(-Dplatforms=$PLATFORMS -Dglx=disabled -Degl=enabled -Dgbm=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled
		-Dgallium-drivers=zink -Dvulkan-drivers=webgpu -Dllvm=disabled) ;;
*)	PROFILE=(-Dplatforms=$PLATFORMS -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
		-Dgallium-drivers= -Dvulkan-drivers=webgpu -Dllvm=disabled) ;;
esac
if [ ! -f $O/build.ninja ]; then
	mkdir -p $B
	meson setup $O $TREE --prefix=$B/native-tools \
		-Dbuildtype=debugoptimized -Db_ndebug=false "${PROFILE[@]}" \
		-Dbuild-tests=false -Dvideo-codecs= -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
		-Dzstd=enabled -Dexpat=enabled -Dshader-cache=enabled \
		> $B/native-setup.log 2>&1 || { tail -40 $B/native-setup.log; exit 1; }
elif [ "$(cat $B/native-platforms 2>/dev/null)" != "$PLATFORMS" ]; then
	meson configure $O -Dplatforms=$PLATFORMS > $B/native-setup.log 2>&1 || { tail -40 $B/native-setup.log; exit 1; }
elif [ $W/pc/webgpu_dawn.pc -nt $O/build.ninja ]; then
	meson setup --reconfigure $O $TREE > $B/native-setup.log 2>&1 || { tail -40 $B/native-setup.log; exit 1; }
fi
echo "$PLATFORMS" > $B/native-platforms
ninja -C $O -j$JOBS "$@" 2>&1 | grep -v -E "^\[|^(cc|c\+\+|gcc|g\+\+|emcc|em\+\+) " | tail -40
s=${PIPESTATUS[0]}
[ $s = 0 ] || { echo "native.sh: ninja failed ($s)"; exit $s; }
echo "native.sh: built $O"
