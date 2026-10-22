#!/bin/bash
# The rvgpu deploy gate (tools/rvgpu-gate.sh): a clean stamped wasm passes, a dirty, unstamped or commit-less one
# is refused, by the Node reader and by the Python one alike. No browser, no build: tiny wasm files only.
S="$(cd "$(dirname "$0")/.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
want() { # NAME EXPECTED-EXIT cmd...
	local name=$1 exp=$2; shift 2
	"$@" > "$T/out" 2>&1
	local got=$?
	if [ "$got" = "$exp" ]; then echo "ok   $name"; else echo "FAIL $name: exit $got, wanted $exp"; cat "$T/out"; fail=1; fi
}
printf '\0asm\1\0\0\0' > "$T/none.wasm"
for k in clean dirty nocommit; do cp "$T/none.wasm" "$T/$k.wasm"; done
C=0123456789abcdef0123456789abcdef01234567
node "$S/tools/rvgpu-stamp.js" --write "$T/clean.wasm" --branch vulkan --commit $C --mesa m --dawn d --patch p --dirty false > /dev/null
node "$S/tools/rvgpu-stamp.js" --write "$T/dirty.wasm" --branch vulkan --commit $C --mesa m --dawn d --patch p --dirty true > /dev/null
node "$S/tools/rvgpu-stamp.js" --write "$T/nocommit.wasm" --branch vulkan --commit unknown --mesa m --dawn d --patch p --dirty false > /dev/null
for reader in node py; do
	if [ $reader = py ]; then G="env PATH=/usr/bin:/bin"; else G=env; fi
	# (py: a PATH without Node, so the gate takes its Python branch)
	if [ $reader = py ] && env PATH=/usr/bin:/bin sh -c "command -v node" > /dev/null; then echo "skip py: Node is in /usr/bin"; continue; fi
	want "$reader clean passes" 0 $G "$S/tools/rvgpu-gate.sh" "$T/clean.wasm"
	want "$reader dirty refused" 2 $G "$S/tools/rvgpu-gate.sh" "$T/dirty.wasm"
	want "$reader nocommit refused" 2 $G "$S/tools/rvgpu-gate.sh" "$T/nocommit.wasm"
	want "$reader unstamped refused" 2 $G "$S/tools/rvgpu-gate.sh" "$T/none.wasm"
	want "$reader missing refused" 2 $G "$S/tools/rvgpu-gate.sh" "$T/nope.wasm"
done
want "line" 0 "$S/tools/rvgpu-gate.sh" --line "$T/clean.wasm"
grep -q "^vulkan@$C mesa m dawn d patch p$" "$T/out" && echo "ok   line text" || { echo "FAIL line text"; cat "$T/out"; fail=1; }
# the Python reader on its own, whatever Node is here
want "python clean" 0 python3 "$S/tools/rvgpu-stamp-check.py" "$T/clean.wasm"
want "python dirty" 2 python3 "$S/tools/rvgpu-stamp-check.py" "$T/dirty.wasm"
want "python unstamped" 2 python3 "$S/tools/rvgpu-stamp-check.py" "$T/none.wasm"
exit $fail
