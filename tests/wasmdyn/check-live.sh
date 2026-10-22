#!/bin/bash
# DL-05/DL-06: the live site (or any release server) under node-machine, with and without ?dl=1: the files the page loads (runtime,
# kernel, boot image) fetched from URL, the release disk read from it as the page reads it. Prints, from inside the machine, what the
# flag changes: the kernel's command line, /opt/nq-shared, the PATH and XDG_DATA_DIRS the desktop gives its programs, and (flag on) the
# launcher entries the menu cache finds. Nothing but reads from the server.
#   MODE=off|on [URL=https://plasmaontheweb.com] [BOOT=boot image to use instead of the server's] [NM_FRESH=1] tests/wasmdyn/check-live.sh
# Output: build/nm-live/check-MODE.log. ONE node-machine at a time (this waits for the others).
set -e
cd "$(dirname "$0")/../.."
URL=${URL:-https://plasmaontheweb.com}
MODE=${MODE:-off}
N=build/nm-live
mkdir -p $N/rt
for f in linux.js linux-worker.js vmlinux-wasm64_nommu.wasm initramfs-wasm64_nommu.cpio.gz; do
	case $f in linux*.js) to=$N/rt/$f ;; *) to=$N/$f ;; esac
	if [ -n "$NM_FRESH" ] || [ ! -s "$to" ]; then
		curl -sf -m 300 -o "$to" "$URL/$f" || { echo "check-live: cannot fetch $URL/$f" >&2; exit 1; }
	fi
done
BOOT=${BOOT:-$N/initramfs-wasm64_nommu.cpio.gz}
RT=${RT:-$N/rt}   # (RT=a runtime directory with a newer linux.js and linux-worker.js, to try them against the live kernel and disks)
ARGS=(--runtime $RT --kernel $N/vmlinux-wasm64_nommu.wasm --initramfs $BOOT --timeout 300000 --server "$URL")
[ $MODE = panel ] || ARGS+=(--append autostart=0)
if [ $MODE = panel ]; then
	# the default page: what dev.html does by itself (rvdl=1 and the nqs disk), the desktop starting by itself as it does for a visitor
	if [ -n "$DL0" ]; then   # (DL0=1: the same with ?dl=0, the emulated desktop: how long the panel takes without the native apps)
		ARGS+=(--disk release,home --query "?dl=0" --timeout 600000 --hook tests/wasmdyn/widgets-hook.js)
	else
		ARGS+=(--disk release,home,nqs --query "?dl=1" --dl-manifest "$URL/dl/libs.json" --append rvdl=1 --timeout 600000 --hook tests/wasmdyn/widgets-hook.js)
	fi
	export WL_DUMP_DIR=$N/panel-shots WL_DUMP_MAX=2 WL_DUMP_ACTIONS="${WL_DUMP_ACTIONS-wait:100|shot:desktop|click:82,738|windows:1,900|settle:10|shot:ss|click:134,738|windows:2,900|settle:10|shot:fm}"
	[ -z "$DL0" ] || { export WL_DUMP_DIR=$N/panel-shots-dl0; WL_DUMP_ACTIONS="wait:100|shot:desktop|wait:100|shot:desktop2"; }
	mkdir -p $N/panel-shots $N/panel-shots-dl0
elif [ $MODE = on ]; then
	ARGS+=(--disk release,home,nqs --query "?dl=1" --dl-manifest "$URL/dl/libs.json")
	ARGS+=(--append rvdl=1)
else
	ARGS+=(--disk release,home --query "?dl=0")
fi
# (what the guest says, typed into its shell: no $(...) there. `desktop wayland sh -c ...` prints the environment desktop gives its
# programs; the menu cache is built the way the launcher's KService does, the strings in it are UTF-16, tr drops the NULs.)
ENVC="debian \"desktop wayland sh -c 'echo ENV-XDG=\\\$XDG_DATA_DIRS; echo ENV-PATH=\\\$PATH; echo ENV-NQSRUN=; command -v nqs-run'\""
SEE="for f in /root/.cache/ksycoca6_*; do echo \\\$f; tr -d '[:cntrl:]' < \\\$f | grep -o -E '(KCalc|Konsole|Dolphin|System Settings|Kate|KWrite|Okular|Gwenview|Ark) \\(native\\)' | sort -u; done"
KSY="debian \"rm -f /root/.cache/ksycoca6_*; export XDG_MENU_PREFIX=plasma-; XDG_DATA_DIRS=/usr/local/share:/usr/share:/opt/nq-shared/share kbuildsycoca6 --noincremental; echo KSY=; $SEE; echo KSY-END\""
CMD="echo MODE-$MODE; cat /proc/cmdline; echo DEVWDC=; ls /dev/wdc 2>&1; echo OPT=; ls /opt 2>&1; echo DEBIAN-OPT=; ls /debian/opt 2>&1; echo MOUNTS=; grep nq-shared /proc/mounts; $ENVC"
[ $MODE = on ] && CMD="$CMD; ls /opt/nq-shared /debian/opt/nq-shared; $KSY"
CMD="$CMD; echo LIVE-CHECK-DONE"
# (up to two node-machines at once while the Mac has more than 40 % memory free)
[ $MODE = panel ] && CMD="sleep 330; echo PS-BEGIN; ps; echo PS-END; echo LIVE-CHECK-DONE"
echo "starting $MODE" >&2
${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/nm-slot.sh perl -e 'alarm shift; exec @ARGV' ${ALARM:-320} node --experimental-wasm-memory64 tools/node-machine.js "${ARGS[@]}" "$CMD" 2>&1 | tee $N/check-$MODE.log
