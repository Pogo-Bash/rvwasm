#!/bin/bash
# The native programs as the DEFAULT desktop, under node-machine against the local build VM's dev server (disks release, home and a
# nqs disk NQS=nqs-all by default), with this tree's /init, desktop script and the guest-menu-check.sh in the boot image (as
# docker/patch-dist.sh puts them into the dist's). What the page does by itself (rvdl=1 on the kernel's command line and the third
# disk unless ?dl=0) is given here by hand: node-machine has no page.
#   SCENARIO=new|old|off|panel [NQS=nqs-all] [WL_DUMP_ACTIONS=...] tests/wasmdyn/check-default.sh [SECONDS]
#     new    a fresh home: the menu after the first boot (each program once, no "(native)", the nqs share before Debian's)
#     old    a home disk whose menu cache is from an older nqs disk (stamps from it): made again, all nine there
#     off    ?dl=0 after the cache was made with the native entries: no nqs disk, the cache made again without them
#     mime   open a pdf, a png and a zip as Dolphin does (kde-open): which program started
#     panel  no script: the desktop (autostart) and its panel, for wl-dump actions (clicks on the pinned launchers)
# Output build/nm-live/default-SCENARIO.log (PNGs in build/nm-live/default-shots-SCENARIO). Waits for a node-machine slot (nm-slot.sh).
set -e
cd "$(dirname "$0")/../.."
N=build/nm-live
SCENARIO=${SCENARIO:-new}
AS=autostart=0   # (--append takes one value: the last one wins)
[ $SCENARIO = panel ] && AS=   # (the desktop starts itself, as it does for a visitor)
NQS=${NQS:-nqs-all}
mkdir -p $N/rt28 $N/default-shots-$SCENARIO
# the runtime: the live one plus whatever tools/patch-runtime.py has that it lacks (item 28: dl on by default, os.dl.warm)
cp $N/rt/linux.js $N/rt/linux-worker.js $N/rt28/
python3 tools/patch-runtime.py $N/rt28 > /dev/null
python3 - "$N/guest-default.cpio.gz" <<'PY'
import gzip, os, sys
def hdr(name, mode, data, ino):
    nb = name.encode() + b"\0"
    b = b"070701" + b"".join(b"%08X" % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nb), 0)) + nb
    b += b"\0" * ((4 - len(b) % 4) % 4) + data
    return b + b"\0" * ((4 - len(data) % 4) % 4)
buf, ino = b"", 1
for name, src in (("init", "tools/initramfs-init"), ("usr", None), ("usr/lib", None), ("usr/lib/wlserver", None),
                  ("usr/lib/wlserver/desktop", "tools/x/desktop"), ("menu-check.sh", "tests/wasmdyn/guest-menu-check.sh"), ("mime-check.sh", "tests/wasmdyn/guest-mime-check.sh"), ("konsole-check.sh", "tests/wasmdyn/guest-konsole-check.sh")):
    buf += hdr(name, 0o100755 if src else 0o040755, open(src, "rb").read() if src else b"", ino)
    ino += 1
open(sys.argv[1] + ".tmp", "wb").write(gzip.compress(buf + hdr("TRAILER!!!", 0, b"", 0), mtime=0))
os.replace(sys.argv[1] + ".tmp", sys.argv[1])
PY
S=${SERVER:-http://localhost:8000}   # (SERVER=https://plasmaontheweb.com: the live disks and libraries instead of the build VM's)
ARGS=(--runtime $N/rt28 --kernel $N/vmlinux-wasm64_nommu.wasm --initramfs $N/initramfs-wasm64_nommu.cpio.gz --initramfs $N/guest-default.cpio.gz
	--timeout 900000 --server $S --hook tests/wasmdyn/widgets-hook.js)
export WL_DUMP_DIR=$N/default-shots-$SCENARIO WL_DUMP_MAX=3
CHK="cat /proc/cmdline; ls /sys/block; grep nq-shared /proc/mounts; ls /opt/nq-shared; debian true; cp /menu-check.sh /debian/tmp/chk.sh"   # (the first `debian` mounts the chroot's /tmp)
if [ $SCENARIO = off ]; then
	ARGS+=(--disk release,home --query "?dl=0" --append "$AS")
else
	ARGS+=(--disk release,home,${LIVE_NQS:-$NQS} --query "?dl=1" --append "rvdl=1 $AS")
	if [ "$S" = http://localhost:8000 ]; then
		PORT=$((20000 + RANDOM % 20000))
		python3 tests/wasmdyn/serve-dir.py ${DL:-build/nm-live/$NQS-dl} $PORT > /dev/null 2>&1 &
		trap 'kill $! 2> /dev/null' EXIT
		sleep 1
		ARGS+=(--dl-manifest http://127.0.0.1:$PORT/libs.json)
	else
		ARGS+=(--dl-manifest $S/dl/libs.json)
	fi
fi
# (the seeds: an old cache is made without the nqs share and stamped with another build; the off cache is stamped as if native entries were in it)
SEED="debian \"mkdir -p /root/.cache; rm -f /root/.cache/ksycoca6_*; XDG_MENU_PREFIX=plasma- XDG_DATA_DIRS=/usr/local/share:/usr/share kbuildsycoca6 --noincremental; echo plasma- > /root/.cache/rv-ksycoca-menu; echo 111 222 > /root/.cache/rv-ksycoca-nqs\""
CLEAN="debian \"rm -f /root/.cache/ksycoca6_* /root/.cache/rv-ksycoca-*\""
case $SCENARIO in
new) CMD="$CHK; $CLEAN; echo SCENARIO-new; debian 'desktop wayland sh /tmp/chk.sh new'; echo CHECK-DONE" ;;
old) CMD="$CHK; $SEED; echo SCENARIO-old; debian 'desktop wayland sh /tmp/chk.sh old'; echo CHECK-DONE" ;;
off) CMD="$CHK; $SEED; echo SCENARIO-off; ls /dev/wdc 2>&1; debian 'desktop wayland sh /tmp/chk.sh off'; echo CHECK-DONE" ;;
konsole) CMD="$CHK; cp /konsole-check.sh /debian/tmp/kchk.sh; echo SCENARIO-konsole; debian 'desktop wayland env PROG=${PROG:-konsole} KCHK_WAIT=${KCHK_WAIT:-90} ${NATIVE_PERL:+RV_NATIVE_PERL=$NATIVE_PERL} sh /tmp/kchk.sh'; echo CHECK-DONE" ;;
mime) CMD="$CHK; cp /mime-check.sh /debian/tmp/mime.sh; echo SCENARIO-mime; debian 'desktop wayland sh /tmp/mime.sh'; echo CHECK-DONE" ;;
panel) CMD="sleep ${1:-300}; echo PS-BEGIN; ps; echo PS-END; echo CHECK-DONE" ;;
esac
[ $SCENARIO = panel ] && WL_DUMP_ACTIONS=${WL_DUMP_ACTIONS-"wait:120|shot:desktop|click:82,738|windows:1,900|settle:10|shot:ss|click:134,738|windows:2,900|settle:10|shot:fm"}
[ -n "$WL_DUMP_ACTIONS" ] && export WL_DUMP_ACTIONS
echo "starting $SCENARIO" >&2
${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/nm-slot.sh perl -e 'alarm shift; exec @ARGV' 900 node --experimental-wasm-memory64 $NODE_FLAGS tools/node-machine.js "${ARGS[@]}" "$CMD" 2>&1 | tee $N/default-$SCENARIO.log
