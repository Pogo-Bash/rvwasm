#!/bin/bash
# The deploy rule for the WebGPU executor (docs: plan VK-05): rvgpu.wasm goes into a release only when it carries
# a clean build stamp (gpu/webgpu/build/wasm.sh writes it; tools/rvgpu-stamp.js reads it). Used by tools/pack-www.sh
# and docker/release.sh.
#
#   tools/rvgpu-gate.sh [--line] FILE.wasm    exit 0: stamped, clean, full commit sha (--line: print a RELEASE.txt line)
#                                             exit 2: refused (the reason on stderr)
# Runs tools/rvgpu-stamp.js --require-clean when Node is here; the build container and the build VM have only Python,
# so there tools/rvgpu-stamp-check.py applies the same rule (tests/rvgpu-gate.sh checks the two agree).
set -e
S="$(cd "$(dirname "$0")/.." && pwd)"
line=
[ "$1" != --line ] || { line=--line; shift; }
f=${1:?usage: rvgpu-gate.sh [--line] FILE.wasm}
[ -e "$f" ] || { echo "rvgpu-gate: no $f" >&2; exit 2; }
if command -v node > /dev/null; then
	node "$S/tools/rvgpu-stamp.js" --require-clean "$f" > /dev/null || exit 2
	[ -z "$line" ] || python3 "$S/tools/rvgpu-stamp-check.py" --line "$f"
elif [ -n "$line" ]; then
	python3 "$S/tools/rvgpu-stamp-check.py" --line "$f"
else
	python3 "$S/tools/rvgpu-stamp-check.py" "$f" > /dev/null
fi
