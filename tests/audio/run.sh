#!/bin/bash
# Run a command on the machine under Node (tools/node-machine.js) with this tree's kernel (the sound card), runtime
# and rv, and tools/audio-consumer.js playing the card into build/audio-out.wav (its statistics on standard error).
#   tests/audio/run.sh 'COMMAND' [TIMEOUT_MS]
# Inputs, in build/nm/, made by tests/audio/prepare.sh:
#   vmlinux.wasm         the kernel built with patches/kernel-audio.diff
#   rt/                  the runtime (linux.js, linux-worker.js) patched by this tree's tools/patch-runtime.py
#   initramfs.cpio.gz    a staged boot image (glibc in /lib for riscv64 programs)
#   overlay.cpio.gz      tests/audio/overlay.sh's: aplay & co, alsa-lib, this tree's rv and /init
# Environment: AUDIO_* (tools/audio-consumer.js), APPEND (more for the kernel's command line), V=1 (verbose),
# DISK=NAME (the dev server's disk, as dev.html?disk=NAME: Debian in /debian, what is written kept in memory only),
# NET=1 (the network relay), NM_INITRAMFS=FILE (another boot image: tests/audio/private-stage.sh's),
# HOOK=FILE.js (another hook instead of tools/audio-consumer.js), EXTRA=DIR (its files go into the machine's root too, after the overlay).
# Only one node-machine at a time on this computer: this waits for another to finish.
set -e
cd "$(dirname "$0")/../.."
N=build/nm
while pgrep -f node-machine.js > /dev/null; do
	echo "(another node-machine is running: waiting)" >&2
	sleep 10
done
opts=(--hook "${HOOK:-tools/audio-consumer.js}")
[ -n "$V" ] && opts+=(--verbose)
[ -n "$APPEND" ] && opts+=(--append "$APPEND")
[ -n "$DISK" ] && opts+=(--disk "$DISK")
[ -n "$NET" ] && opts+=(--net)
if [ -n "$EXTRA" ]; then
	(cd "$EXTRA" && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2>/dev/null | gzip -1) > $N/extra.cpio.gz
	opts+=(--initramfs $N/extra.cpio.gz)
fi
# A hard limit on top of the machine's own timeout.
exec perl -e 'alarm 900; exec @ARGV' node --experimental-wasm-memory64 tools/node-machine.js --runtime $N/rt \
	--kernel $N/vmlinux.wasm --initramfs "${NM_INITRAMFS:-$N/initramfs.cpio.gz}" --initramfs $N/overlay.cpio.gz \
	--timeout "${2:-120000}" "${opts[@]}" "$1"
