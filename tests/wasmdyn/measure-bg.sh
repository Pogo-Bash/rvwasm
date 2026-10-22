#!/bin/bash
# The page's background compile of the native libraries (tools/www/dev.html dlBackground), measured under node-machine: a cold home, the
# desktop (autostart) up, then LAUNCH_AT seconds after the machine started the native Konsole, Dolphin, System Settings and Kate one
# after the other (each stopped after 45 s), with MODE=bg the job running from BG_AT seconds (tests/wasmdyn/dl-bg-hook.js: the same text
# as the page's, a mock display) and with MODE=nobg without it. Every line the machine prints and each window the window server shows is
# stamped with the clock; the window of a program is the first slot line with its app id after its MARK-LAUNCH line. Samples every 3 s:
# the Node process's committed wasm code (tools/codemem.sh) and resident set. Output build/nm-live/bg-MODE.{log,mem}.
#   MODE=bg|nobg [KEEP=n: keep Modules of libraries with order below n; default all] [LAUNCH_AT=240] [BG_AT=170] [NQS=nqs-all] tests/wasmdyn/measure-bg.sh
set -e
cd "$(dirname "$0")/../.."
N=build/nm-live
MODE=${MODE:-bg}
NQS=${NQS:-nqs-all}
LAUNCH_AT=${LAUNCH_AT:-240}
BG_AT=${BG_AT:-170}
mkdir -p $N/rt28 $N/bg-shots
cp $N/rt/linux.js $N/rt/linux-worker.js $N/rt28/
python3 tools/patch-runtime.py $N/rt28 > /dev/null
python3 - "$N/guest-bg.cpio.gz" <<'PY'
import gzip, os, sys
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
open(sys.argv[1] + ".tmp", "wb").write(gzip.compress(buf + hdr("TRAILER!!!", 0, b"", 0), mtime=0))
os.replace(sys.argv[1] + ".tmp", sys.argv[1])
PY
MEM="grep -E 'MemFree|Cached' /proc/meminfo"
RUN="for a in konsole dolphin systemsettings kate; do echo MARK-LAUNCH-\$a; debian \"desktop wayland nqs-run \$a\" > /tmp/\$a.out 2>&1 & sleep 45; echo MARK-STOP-\$a; $MEM; debian 'pkill -x ld-wasm; pkill -x kioworker'; sleep 4; done"
CMD="echo MARK-BOOTED; sleep $LAUNCH_AT; echo MARK-LAUNCHING; $MEM; $RUN; echo MARK-DONE"
PORT=$((20000 + RANDOM % 20000))
python3 tests/wasmdyn/serve-dir.py ${DL:-$N/$NQS-dl} $PORT > /dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2> /dev/null' EXIT
sleep 1
ARGS=(--verbose --runtime $N/rt28 --kernel $N/vmlinux-wasm64_nommu.wasm --initramfs $N/initramfs-wasm64_nommu.cpio.gz --initramfs $N/guest-bg.cpio.gz
	--timeout 900000 --server http://localhost:8000 --hook tests/wasmdyn/dl-bg-hook.js --disk release,home,$NQS --query "?dl=1" --append rvdl=1
	--dl-manifest http://127.0.0.1:$PORT/libs.json)
export WL_DUMP_DIR=$N/bg-shots WL_DUMP_MAX=1
[ $MODE = bg ] && export DLBG_URL=http://127.0.0.1:$PORT/libs.json DLBG_DELAY=$BG_AT ${KEEP:+DLBG_KEEP=$KEEP}
stamp='use Time::HiRes qw(time); $|=1; while(<STDIN>){ printf "%.1f %s", time, $_ }'
: > $N/bg-$MODE.mem
(
	for i in $(seq 1 600); do P=$(pgrep -f "^node .*guest-bg.cpio.gz" | head -1); [ -n "$P" ] && break; sleep 0.5; done
	while kill -0 "$P" 2> /dev/null; do
		echo "$(perl -MTime::HiRes=time -e 'printf "%.1f", time') $(tools/codemem.sh $P) $(( $(ps -o rss= -p $P) / 1024 ))" >> $N/bg-$MODE.mem
		sleep 3
	done
) &
SAMPLER=$!
echo "starting $MODE; load: $(uptime | sed "s/.*load averages*: //")" >&2
${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/nm-slot.sh perl -e 'alarm shift; exec @ARGV' 900 node --experimental-wasm-memory64 tools/node-machine.js "${ARGS[@]}" "$CMD" 2>&1 | perl -e "$stamp" | tee $N/bg-$MODE.log
wait $SAMPLER 2> /dev/null || true
echo "samples (epoch, MiB committed wasm code, ranges, MiB resident): $N/bg-$MODE.mem" >&2
