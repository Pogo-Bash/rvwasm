#!/bin/bash
# Build one of the reproducers with the build VM's linux-wasm toolchain (tools/wasm-cc) and pack every built
# one into an initramfs piece (under /audit/) that run.sh puts on top of the machine's own.
#   tests/linux-wasm-audit/build.sh NAME...   (NAME.c in this directory)
# The VM sees only the main checkout (RVWASM_MAIN), so the source goes in and the binary comes out through the pipe.
# AUDIT_OUT: where the binaries and the initramfs piece go (default build/linux-wasm-audit; run.sh takes it too).
# WASM_CC: the compiler driver's path in the VM (default: the main checkout's tools/wasm-cc; a tree with its own
# musl gives its own). CFLAGS: more flags (cas_p.c takes -DMUSL_ATOMIC -I<musl>/arch/wasm to test musl's own a_cas_p).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${AUDIT_OUT:-$here/../../build/linux-wasm-audit}
export PATH=$here/../../tools/lima/bin:${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
cc=${WASM_CC:-${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/wasm-cc}
mkdir -p "$out/bin/audit"
for n in "$@"; do
	limactl shell --workdir / rvwasm bash -c "mkdir -p /tmp/aud && cat > /tmp/aud/$n.c && $cc $CFLAGS -o /tmp/aud/$n /tmp/aud/$n.c 2>/dev/null && cat /tmp/aud/$n" \
		< "$here/$n.c" > "$out/bin/audit/$n.tmp" || { echo "build.sh: $n did not build" >&2; rm -f "$out/bin/audit/$n.tmp"; exit 1; }
	mv "$out/bin/audit/$n.tmp" "$out/bin/audit/$n"
	chmod +x "$out/bin/audit/$n"
done
(cd "$out/bin" && find audit | cpio -o -H newc 2>/dev/null | gzip > "$out/audit.cpio.gz")
