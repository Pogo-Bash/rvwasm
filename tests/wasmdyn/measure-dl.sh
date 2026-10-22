#!/bin/bash
# DL-05: what ?dl=1 costs a tab, measured from outside the process (node-machine, no browser). One run = one machine:
#   MODE=on   nqs disk + rvdl=1 + the library manifest compiled at the start (the page's way: os.dl.preload_manifest),
#             then the window server, then native KCalc, then native Konsole beside it;
#   MODE=off  the same machine without the flag (no nqs disk, no manifest): the baseline of the same commands (plain `sleep`s).
# While it runs, every 3 s: the Node process's committed wasm code (tools/codemem.sh: rwx ranges of vmmap, MiB and count) and its
# resident set (MiB), with the clock; every line the machine prints and the window server's events are stamped with the clock
# too. MARK-* lines in the output are the phases. Result: build/wasmdyn/measure-MODE.log (run) and measure-MODE.mem (samples).
#   MODE=on|off [NQS=disk name] [DL=dir of libs.json + libraries] tests/wasmdyn/measure-dl.sh
# ONE node-machine at a time (this waits for the others). Needs the same as check-guest.sh (build/wasmdyn, dev server :8000).
set -e
cd "$(dirname "$0")/../.."
BASE=${BASE:-build/wasmdyn}
MODE=${MODE:-on}
NQS=${NQS:-nqs}
OUT=build/wasmdyn
mkdir -p $OUT/measure-shots
# (the tree's /init and desktop on the boot image, as check-guest.sh and patch-dist.sh do)
python3 - "$OUT/guest.cpio.gz" <<'PY'
import gzip, sys
def hdr(name, mode, data, ino):
    nb = name.encode() + b"\0"
    b = b"070701" + b"".join(b"%08X" % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nb), 0)) + nb
    b += b"\0" * ((4 - len(b) % 4) % 4) + data
    return b + b"\0" * ((4 - len(data) % 4) % 4)
buf, ino = b"", 1
for name, src in (("init", "tools/initramfs-init"), ("usr", None), ("usr/lib", None), ("usr/lib/wlserver", None),
                  ("usr/lib/wlserver/desktop", "tools/x/desktop")):
    buf += hdr(name, 0o100755 if src else 0o040755, open(src, "rb").read() if src else b"", ino)
    ino += 1
open(sys.argv[1], "wb").write(gzip.compress(buf + hdr("TRAILER!!!", 0, b"", 0), mtime=0))
PY
MEM="grep -E 'MemTotal|MemFree|Cached' /proc/meminfo"
if [ $MODE = on ]; then
	K="debian 'desktop wayland nqs-run kcalc' > /tmp/kc.out 2>&1 & KC=\$!"
	O="debian 'desktop wayland nqs-run konsole' > /tmp/ko.out 2>&1 & KO=\$!"
else
	K="sleep 0"; O="sleep 0"
fi
CMD="echo MARK-BOOTED; $MEM; debian 'desktop wayland sleep 900' > /dev/null 2>&1 & i=0; while [ ! -S /debian/tmp/runtime-root/wayland-0 ] && [ \$i -lt 240 ]; do sleep 0.5; i=\$((i+1)); done; echo MARK-SERVER-UP; sleep 20; echo MARK-IDLE; $MEM; echo MARK-KCALC-START; $K; sleep 70; echo MARK-KCALC-SETTLED; $MEM; echo MARK-KONSOLE-START; $O; sleep 90; echo MARK-KONSOLE-SETTLED; $MEM; ps | grep -E 'ld-wasm|kcalc|konsole' | head; echo MARK-END"
# (--verbose: the machine's console as it comes on stderr, so that the MARK lines get their real time; stdout is printed at the end)
ARGS=(--verbose --runtime $BASE/rt --kernel $BASE/vmlinux.wasm --initramfs $BASE/initramfs.cpio.gz --initramfs $OUT/guest.cpio.gz --timeout 600000
	--hook tests/wasmdyn/widgets-hook.js)
export WL_DUMP_DIR=$OUT/measure-shots WL_DUMP_MAX=2
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
while pgrep -f "^node .*tools/[n]ode-machine.js" > /dev/null; do sleep 10; done
echo "starting $MODE; load: $(uptime | sed "s/.*load averages*: //")" >&2
stamp='use Time::HiRes qw(time); $|=1; while(<STDIN>){ printf "%.1f %s", time, $_ }'
: > $OUT/measure-$MODE.mem
(
	# the sampler: wait for the node process, then sample until it ends
	for i in $(seq 1 60); do P=$(pgrep -f "^node .*tools/[n]ode-machine.js" | head -1); [ -n "$P" ] && break; sleep 0.5; done
	while kill -0 "$P" 2> /dev/null; do
		echo "$(perl -MTime::HiRes=time -e 'printf "%.1f", time') $(tools/codemem.sh $P) $(( $(ps -o rss= -p $P) / 1024 ))" >> $OUT/measure-$MODE.mem
		sleep 3
	done
) &
SAMPLER=$!
perl -e 'alarm shift; exec @ARGV' 640 node --experimental-wasm-memory64 tools/node-machine.js "${ARGS[@]}" "$CMD" 2>&1 | perl -e "$stamp" | tee $OUT/measure-$MODE.log
wait $SAMPLER 2> /dev/null || true
echo "samples (epoch, MiB committed wasm code, ranges, MiB resident): $OUT/measure-$MODE.mem" >&2
