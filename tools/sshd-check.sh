#!/bin/bash
# OpenSSH's sshd under rv: a key login, which needs chroot() (sshd-auth enters /run/sshd before
# authentication; without chroot it gives up with "chroot("/run/sshd"): Function not implemented").
# rv runs inside a chroot of ROOT, a Debian riscv64 root with openssh-server in it (a copy: this adds
# host keys, /rvtest/rv and /tmp/rvfix-ak to it), with the host's /proc, /sys and /dev bound in.
# Port 22722, a throwaway key, the host's own ssh client. Needs sudo. Run inside the build VM.
#
# RV must be linked statically (make -C rv O=DIR LDFLAGS=-static): it runs inside ROOT.
#
# Usage: sshd-check.sh RV ROOT

set -u
RV="$(realpath "$1")"
R="$(realpath "$2")"
K="$(mktemp -d)"
sudo mkdir -p "$R/rvtest" "$R/run/sshd" && sudo cp "$RV" "$R/rvtest/rv"
for d in proc sys dev; do mountpoint -q "$R/$d" || sudo mount --bind "/$d" "$R/$d"; done
cleanup() {
	[ -n "${SSHD:-}" ] && sudo kill "$SSHD" 2> /dev/null
	sudo umount "$R/proc" "$R/sys" "$R/dev"
	sudo rm -f "$R/tmp/rvfix-ak"
	rm -rf "$K"
}
trap cleanup EXIT
IN=(sudo chroot "$R" /usr/bin/env -i PATH=/usr/bin:/usr/sbin HOME=/root /rvtest/rv)
"${IN[@]}" /usr/bin/ssh-keygen -A > /dev/null
ssh-keygen -q -t ed25519 -N "" -f "$K/k"
sudo cp "$K/k.pub" "$R/tmp/rvfix-ak"
if ss -ltn | grep -q ":22722 "; then
	echo "sshd: port 22722 is taken"
	exit 1
fi
"${IN[@]}" /usr/sbin/sshd -d -p 22722 -o AuthorizedKeysFile=/tmp/rvfix-ak -o StrictModes=no -o PidFile=none > "$K/sshd.log" 2>&1 &
SSHD=$!
for i in $(seq 90); do grep -q "Server listening" "$K/sshd.log" && break; sleep 2; done
out="$(timeout 600 ssh -i "$K/k" -p 22722 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-o BatchMode=yes root@localhost 'echo login ok; uname -m' 2> /dev/null)"
sleep 2
if [ "$out" == "$(printf 'login ok\nriscv64')" ]; then
	echo "sshd: key login ok"
else
	echo "sshd: FAILED"
	grep -iE "chroot|fatal|error|refused" "$K/sshd.log" | head -10 | sed 's/^/    /'
	exit 1
fi
