#!/bin/bash
# (In the VM; from the Mac: gpu/webgpu/build/vm.sh gpu/webgpu/build/check.sh [TEST...])
# The native test entry: builds (native.sh) and then runs every gpu/webgpu/tests/*.test, or the named ones.
# A test is an executable script; it prints "ok NAME: ..." or "FAIL NAME: ..." lines and exits non-zero on
# failure. It gets in its environment:
#   WT TREE B W        as in env.sh (B/native is the native build)
#   T                  the tests directory
#   WGVK_ICD LVP_ICD   ICD json files of wgvk and lavapipe (this build's, or the integration build's)
# and runs wgvk as VK_DRIVER_FILES=$WGVK_ICD WGVK_DAWN_DRIVER_FILES=$LVP_ICD (Dawn under wgvk on lavapipe).
set -e
. "$(dirname "$0")/env.sh"
mkdir -p $B
[ -n "$NO_BUILD" ] || bash "$(dirname "$0")/native.sh" > $B/check-build.log 2>&1 || { tail -30 $B/check-build.log; exit 1; }
export WT TREE B W T=$WT/gpu/webgpu/tests
export WGVK_ICD=$B/native/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json
export LVP_ICD=$B/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
# (a GPU_NATIVE=min track build has no lavapipe: the integration build's)
[ -f $LVP_ICD ] || LVP_ICD=$W/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
set +e
tests=("$@")
[ ${#tests[@]} = 0 ] && tests=($(cd $T && ls *.test))
failed=0
for t in "${tests[@]}"; do
	t=${t%.test}
	out=$(bash $T/$t.test 2>&1); s=$?
	echo "$out" | grep -E "^(ok|FAIL) " || echo "$out" | tail -15
	if [ $s != 0 ]; then failed=$((failed + 1)); echo "FAIL $t (exit $s)"; fi
done
echo "check.sh: ${#tests[@]} tests, $failed failed"
[ $failed = 0 ]
