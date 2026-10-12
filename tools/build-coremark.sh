#!/bin/bash
# Build CoreMark twice: for riscv64 (to run under rv) and as a native
# Linux/Wasm program (the speed ceiling). Run inside the build VM.
#
# Without floating point in the report (HAS_FLOAT=0): printing a float traps
# in native wasm64 programs. Run with explicit seeds and an iteration count,
# e.g. `coremark 0 0 0x66 2000`; without them CoreMark calibrates itself to
# run for at least ten seconds.

set -e

W="${RVWASM_WORK:-$HOME/rvwasm}"
S="$(realpath "$(dirname "$0")/..")"

[ -d "$W/coremark" ] || git clone -q --depth 1 https://github.com/eembc/coremark "$W/coremark"
cd "$W/coremark"

SRC="core_list_join.c core_main.c core_matrix.c core_state.c core_util.c posix/core_portme.c"
FLAGS="-O2 -I. -Iposix -DPERFORMANCE_RUN=1 -DHAS_FLOAT=0"

mkdir -p "$W/guest-extra" "$W/build-probes"
riscv64-linux-gnu-gcc $FLAGS -DFLAGS_STR='"-O2 riscv64"' -static -o "$W/guest-extra/coremark" $SRC -lrt
"$S/tools/wasm-cc" $FLAGS -DFLAGS_STR='"-O2 wasm64"' -o "$W/build-probes/coremark-wasm" $SRC 2>&1 |
	grep -v -E 'signature mismatch|^>>> |^$|not yet stable' || true
ls -la "$W/guest-extra/coremark" "$W/build-probes/coremark-wasm"
