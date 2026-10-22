#!/bin/bash
# DL-04, the guest side of ?dl=1 under node-machine, as the page would make the machine: with MODE=on the nqs disk is the third
# disk and the kernel's command line has rvdl=1 (dev.html?dl=1); with MODE=off neither (the flag off). This tree's /init and
# desktop script are appended to the boot image, as docker/patch-dist.sh does for the live one.
#   MODE=on|off [PROGRAM=kcalc|konsole] [NQS=disk name] [WL_DUMP_ACTIONS=...] tests/wasmdyn/check-guest.sh [SECONDS]
#                                                          output in build/wasmdyn/guest-NAME.log, PNGs in guest-shots
# Needs build/wasmdyn (rt, vmlinux.wasm, initramfs.cpio.gz: tests/wasmdyn/mkrt.sh and the kernel's build; BASE= another
# such directory), the dev server (:8000) with disks daily, home and (MODE=on) nqs (NQS= another name), and for the
# library manifest DL=DIR (nqs/dl of the build). ONE node-machine at a time.
set -e
cd "$(dirname "$0")/../.."
BASE=${BASE:-build/wasmdyn}
MODE=${MODE:-on}
PROGRAM=${PROGRAM:-kcalc}
NQS=${NQS:-nqs}
OUT=build/wasmdyn
mkdir -p $OUT/guest-shots
python3 - "$OUT/guest.cpio.gz" <<'PY'
import gzip, sys
def hdr(name, mode, data, ino):
    nb = name.encode() + b"\0"
    b = b"070701" + b"".join(b"%08X" % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nb), 0)) + nb
    b += b"\0" * ((4 - len(b) % 4) % 4) + data
    return b + b"\0" * ((4 - len(data) % 4) % 4)
buf, ino = b"", 1
for name, src in (("init", "tools/initramfs-init"), ("usr", None), ("usr/lib", None), ("usr/lib/wlserver", None),
                  ("usr/lib/wlserver/desktop", "tools/x/desktop"), ("native-ab.sh", "tools/native-ab.sh")):
    buf += hdr(name, 0o100755 if src else 0o040755, open(src, "rb").read() if src else b"", ino)
    ino += 1
import os
open(sys.argv[1] + ".tmp", "wb").write(gzip.compress(buf + hdr("TRAILER!!!", 0, b"", 0), mtime=0))
os.replace(sys.argv[1] + ".tmp", sys.argv[1])   # (atomic: another run may be starting from it)
PY
# What the guest checks, typed into its shell (no $(...): that shell cannot fork for it). `desktop wayland sh -c ...` is the
# first client of a server of its own that ends with it; it prints the environment desktop gives its clients.
ENVCHECK="debian \"desktop wayland sh -c 'echo ENV-XDG=\\\$XDG_DATA_DIRS; echo ENV-PATH=\\\$PATH; echo ENV-NQSRUN=; command -v nqs-run; echo ENV-RUNTIME=\\\$XDG_RUNTIME_DIR'\""
CHECK="echo CHECK-$MODE; cat /proc/cmdline; ls /dev/wdb /dev/wdc /dev/ptmx; echo OPT-BEGIN; ls /opt /debian/opt; echo OPT-END; grep nq-shared /proc/mounts; ls /opt/nq-shared /debian/opt/nq-shared; $ENVCHECK"
if [ $MODE = on ]; then
	START="debian 'desktop wayland sleep 900' > /dev/null 2>&1 & i=0; while [ ! -S /debian/tmp/runtime-root/wayland-0 ] && [ \$i -lt 240 ]; do sleep 0.5; i=\$((i+1)); done; echo SERVER-UP"
	# (the launcher's way: nqs-run through the chroot, joining the running server)
	RUN="debian 'desktop wayland nqs-run $PROGRAM' > /tmp/prog.out 2>&1 & KP=\$!; sleep ${1:-60}; ps; kill \$KP 2>/dev/null; echo PROG-OUT-BEGIN; tail -40 /tmp/prog.out; echo PROG-OUT-END; echo GUEST-DONE"
	CMD="$CHECK; $START; $RUN"
	if [ $PROGRAM = ab ]; then
		# tools/native-ab.sh (emulated vs native, launch to first frame) as the owner runs it in a terminal of the desktop: copied into the
		# chroot's /tmp, run through `desktop` (its environment), for APP (dolphin|systemsettings|both), RUNS runs
		RUN="cp /native-ab.sh /debian/tmp/ab.sh; debian 'desktop wayland bash ${AB_X:+-x} /tmp/ab.sh ${AB_APP:-dolphin} ${AB_RUNS:-1}' > /tmp/ab.out 2>&1; echo AB-BEGIN; tail -n ${AB_TAIL:-30} /tmp/ab.out; echo AB-END; head -c 600 /debian/tmp/ab-*-native-1.log; echo GUEST-DONE"
		CMD="$START; $RUN"   # (no CHECK: the console line is limited to ~1000 characters)
	fi
	if [ $PROGRAM = check ]; then
		# (no program: the launcher's entries as the menu cache sees them, once without the share and once with it as `desktop`
		# adds it, built the way KService does when it finds the cache stale; and the shell Konsole starts, in the chroot and out)
		# (the cache has its strings as UTF-16: tr drops the NULs and the other control characters, then grep sees plain text)
		SEE="for f in /root/.cache/ksycoca6_*; do echo \\\$f; tr -d '[:cntrl:]' < \\\$f | grep -o -E '(KCalc|Konsole) \\(native\\)' | sort -u; done"
		KSY="debian \"rm -f /root/.cache/ksycoca6_*; export XDG_MENU_PREFIX=plasma- XDG_DATA_DIRS=/usr/local/share:/usr/share; kbuildsycoca6 --noincremental; echo KSY-WITHOUT=; $SEE; XDG_DATA_DIRS=\\\$XDG_DATA_DIRS:/opt/nq-shared/share kbuildsycoca6; echo KSY-WITH=; $SEE; echo KSY-END\""
		SH="debian '/opt/nq-shared/bin/nqs-shell -c \"echo NQSSHELL-CHROOT-OK\"'; /opt/nq-shared/bin/nqs-shell -c 'echo NQSSHELL-NATIVE-OK'"
		CMD="$CHECK; $SH; $KSY; echo GUEST-DONE"
	fi
else
	CMD="$CHECK; echo GUEST-DONE"
fi
if [ $PROGRAM = check ] || [ $PROGRAM = ab ]; then
	ACTIONS=""
elif [ $PROGRAM = konsole ]; then
	ACTIONS="windows:1,900|settle:4|shot:konsole|click:300,300|type:echo hello; ls /|key:Enter|settle:3|shot:echo"
else
	ACTIONS="windows:1,600|settle:3|shot:kcalc|click:185,404|click:392,308|click:288,404|click:392,436|settle:2|shot:2plus3|type:*4|key:Enter|settle:2|shot:times4"
fi
export WL_DUMP_DIR=$OUT/guest-shots-$PROGRAM WL_DUMP_MAX=3; mkdir -p $OUT/guest-shots-$PROGRAM
[ $MODE = on ] && export WL_DUMP_ACTIONS="${WL_DUMP_ACTIONS-$ACTIONS}"
ARGS=(--runtime $BASE/rt --kernel $BASE/vmlinux.wasm --initramfs $BASE/initramfs.cpio.gz --initramfs $OUT/guest.cpio.gz --timeout 600000
	--hook tests/wasmdyn/widgets-hook.js)
if [ $MODE = on ]; then
	ARGS+=(--disk daily,home,$NQS --query "?dl=1" --append rvdl=1)
	if [ -n "$DL" ]; then
		PORT=$((20000 + RANDOM % 20000))
		python3 -m http.server $PORT --bind 127.0.0.1 --directory "$DL" > /dev/null 2>&1 &
		trap 'kill $! 2> /dev/null' EXIT
		sleep 1
		ARGS+=(--dl-manifest http://127.0.0.1:$PORT/libs.json)
	fi
else
	ARGS+=(--disk daily,home --query "?dl=0")
fi
# (the machine-wide slots: at most two node-machines at once, >40% memory free: nm-slot.sh)
echo "starting $MODE $PROGRAM" >&2
${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/nm-slot.sh perl -e 'alarm shift; exec @ARGV' 640 node --experimental-wasm-memory64 tools/node-machine.js "${ARGS[@]}" "$CMD" | tee $OUT/guest-$MODE-$PROGRAM.log
