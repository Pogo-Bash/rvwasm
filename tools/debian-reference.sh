#!/bin/bash
# The reference output of tests/debian.sh: Debian's bash running it under
# qemu-riscv64 in a chroot of ~/rvwasm/debian-root (qemu-user-static's
# binfmt_misc entry has the F flag, so it works there). Needs sudo for the
# chroot and for mounting /proc in it. Run inside the build VM.

set -e

S="$(realpath "$(dirname "$0")/..")"
ROOT="${1:-${RVWASM_WORK:-$HOME/rvwasm}/debian-root}"

sudo cp "$S/tests/debian.sh" "$ROOT/tmp/debian.sh"
sudo mount -t proc none "$ROOT/proc"
trap 'sudo umount "$ROOT/proc"; sudo rm -f "$ROOT/tmp/debian.sh"' EXIT
[ -e "$ROOT/dev/fd" ] || sudo ln -s /proc/self/fd "$ROOT/dev/fd"
sudo chroot "$ROOT" /usr/bin/env -i HOME=/root TERM=xterm \
	PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin /bin/bash /tmp/debian.sh 2>&1
