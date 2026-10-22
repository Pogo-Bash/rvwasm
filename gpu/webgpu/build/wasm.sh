#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/build/wasm.sh [ninja targets])
# The Emscripten Mesa build, from the same tree as native.sh: wgvk (static, on emdawnwebgpu) and zink's
# Gallium driver (for vfront), linked by meson into rvgpu.js/rvgpu.wasm (src/webgpu/rvgpu/meson.build).
# Compiled against emdawnwebgpu's headers; the port itself (--use-port) only at the final link (it needs C++,
# and meson's link checks are C). Default targets: rvgpu.js and zink; or name ninja targets. Incremental.
# Output: $B/wasm/src/webgpu/rvgpu/rvgpu.{js,wasm}.
# VK-04: reproducible and stamped. The compilers map the build's absolute paths (-ffile-prefix-map, below), so
# a build from another worktree or build name gives the same bytes; and the last step appends the custom section
# "rvgpu-stamp" (branch, commit, Mesa and Dawn pins, dirty; tools/rvgpu-stamp.js reads it) to rvgpu.wasm. The
# source of the stamp comes from vm.sh (RVGPU_BRANCH/COMMIT/DIRTY); a run without it stamps commit "unknown",
# dirty: a build the deploy scripts refuse. Deploy rule: only a stamped, clean rvgpu.wasm goes to the site
# (tools/rvgpu-stamp.js --require-clean; docker/release.sh enforces it from VK-05 on).
set -e
. "$(dirname "$0")/env.sh"
bash "$(dirname "$0")/tree.sh"
bash "$(dirname "$0")/vkr-tree.sh"	# (T5: vkr's sources, src/vkr-src)
source ~/emsdk/emsdk_env.sh > /dev/null 2>&1
PORT=$W/emdawnwebgpu/emdawnwebgpu_pkg/emdawnwebgpu.port.py
[ -f $PORT ] || bash "$(dirname "$0")/emdawnwebgpu.sh"
O=$B/wasm
mkdir -p $B
# (Absolute paths of this build, and meson's relative ones (../../../trees/NAME/...), reach the binary through __FILE__/assert strings; the last matching map wins,
# so the longest prefixes go last. A fixed SOURCE_DATE_EPOCH covers any timestamp a tool embeds.)
export SOURCE_DATE_EPOCH=1760000000
PM="-ffile-prefix-map=$HOME=/home -ffile-prefix-map=$W=/gpu -ffile-prefix-map=$WT=/wt -ffile-prefix-map=$B=/b -ffile-prefix-map=$TREE=/mesa -ffile-prefix-map=$(realpath -m --relative-to=$O $TREE)=/mesa"
cat > $B/emscripten.cross.new <<CROSS
[binaries]
c = 'emcc'
cpp = 'em++'
ar = 'emar'
strip = 'emstrip'
pkg-config = 'false'

[built-in options]
c_args = ['-I$W/emdawnwebgpu/emdawnwebgpu_pkg/webgpu/include', '-pthread'$(for f in $PM; do printf ", '%s'" $f; done)]
cpp_args = ['-I$W/emdawnwebgpu/emdawnwebgpu_pkg/webgpu/include', '-pthread'$(for f in $PM; do printf ", '%s'" $f; done)]
c_link_args = ['-pthread']
cpp_link_args = ['-pthread']

[properties]
emdawnwebgpu_port = '$PORT'

[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'
CROSS
cmp -s $B/emscripten.cross.new $B/emscripten.cross && rm $B/emscripten.cross.new || mv $B/emscripten.cross.new $B/emscripten.cross
if [ ! -f $O/build.ninja ]; then
	meson setup $O $TREE --cross-file $B/emscripten.cross \
		-Dbuildtype=release -Db_ndebug=true -Dc_thread_count=0 -Dcpp_thread_count=0 \
		-Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
		-Dgallium-drivers=zink -Dvulkan-drivers=webgpu -Dllvm=disabled -Dmesa-clc=auto \
		-Dbuild-tests=false -Dvideo-codecs= -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
		-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled \
		> $B/wasm-setup.log 2>&1 || { tail -40 $B/wasm-setup.log; exit 1; }
fi
# (c_thread_count: meson's own Emscripten option, the pthread Worker pool made at start-up; wgvk starts no
# threads. Builds set up before it was here get it now.)
if grep -q "PTHREAD_POOL_SIZE=4" $O/build.ninja; then
	meson configure --clearcache $O -Dc_thread_count=0 -Dcpp_thread_count=0 > /dev/null
	meson setup --reconfigure $O $TREE > $B/wasm-setup.log 2>&1
fi
[ $# = 0 ] && set -- src/webgpu/rvgpu/rvgpu.js src/gallium/drivers/zink/libzink.a
ninja -C $O -j$JOBS "$@" 2>&1 | grep -v -E "^\[|^(cc|c\+\+|gcc|g\+\+|emcc|em\+\+|.*/emcc|.*/em\+\+) " | tail -40
s=${PIPESTATUS[0]}
[ $s = 0 ] || { echo "wasm.sh: ninja failed ($s)"; exit $s; }
# The stamp (idempotent: an existing section is replaced).
if [ -f $O/src/webgpu/rvgpu/rvgpu.wasm ]; then
	dawn=$(grep -v '^#' $WT/gpu/webgpu/build/dawn.pin | head -1 | cut -d' ' -f1)
	patch=$(sha1sum < $WT/patches/mesa-page.diff | cut -c1-40)
	node $WT/tools/rvgpu-stamp.js --write $O/src/webgpu/rvgpu/rvgpu.wasm --branch "${RVGPU_BRANCH:-unknown}" \
		--commit "${RVGPU_COMMIT:-unknown}" --mesa $PIN --dawn $dawn --patch $patch --dirty "${RVGPU_DIRTY:-true}"
fi
echo "wasm.sh: built $O"
ls -la $O/src/webgpu/rvgpu/rvgpu.js $O/src/webgpu/rvgpu/rvgpu.wasm 2>/dev/null || true
