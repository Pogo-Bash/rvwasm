#!/bin/bash
# Run shell commands on the machine under Node (tools/node-machine.js), with the reproducers built by build.sh
# in /audit/. The kernel, runtime and initramfs are copies of what the build VM has staged (fetched once).
#   tests/linux-wasm-audit/run.sh 'COMMAND' [TIMEOUT_MS]
# NM: a directory with rt/ (the runtime), vmlinux.wasm and initramfs.cpio.gz to use instead (a tree's own builds);
# EXTRA: more initramfs pieces, put after the machine's own (their files win). APPEND: more for the kernel's
# command line (e.g. wasm_pool=1).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${AUDIT_OUT:-$here/../../build/linux-wasm-audit}
nm=${NM:-$out/nm}
export PATH=$here/../../tools/lima/bin:${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
if [ -z "$NM" ] && [ ! -f "$out/nm/vmlinux.wasm" ]; then
	mkdir -p "$out/nm/rt"
	for f in linux.js linux-worker.js; do
		limactl shell --workdir / rvwasm bash -c "cat ~/rvwasm/www/$f" > "$out/nm/rt/$f"
	done
	limactl shell --workdir / rvwasm bash -c 'cat ~/rvwasm/www/vmlinux-wasm64_nommu.wasm' > "$out/nm/vmlinux.wasm"
	limactl shell --workdir / rvwasm bash -c 'cat ~/rvwasm/www/initramfs-wasm64_nommu.cpio.gz' > "$out/nm/initramfs.cpio.gz"
fi
# A hard limit of 10 minutes on top of the machine's own timeout.
exec perl -e 'alarm 600; exec @ARGV' node --experimental-wasm-memory64 "$here/../../tools/node-machine.js" \
	--runtime "$nm/rt" --kernel "$nm/vmlinux.wasm" --initramfs "$nm/initramfs.cpio.gz" \
	$(for e in $EXTRA; do echo --initramfs "$e"; done) ${APPEND:+--append "$APPEND"} --initramfs "$out/audit.cpio.gz" --timeout "${2:-60000}" "$1"
