#!/bin/bash
# native-ab.sh: time Debian's (emulated) Dolphin / System Settings / Kate against the native ones, launch to window, by hand.
# Run it in an emulated terminal of the desktop of dev.html?dl=1 (xterm, or Debian's Konsole; NOT the native Konsole: it stops
# native apps at the end of a run). It needs the nqs disk (nqs-run on the PATH: only with ?dl=1).
#
#   native-ab.sh [dolphin|systemsettings|kate|kwrite|okular|gwenview|ark|both] [RUNS]    (default: both = dolphin, systemsettings, kate, okular, gwenview; 2 runs each:
#                                                                      run 1 is the first launch of each, run 2 a second launch)
#
# HOW IT WORKS: for each program and each of the two versions the script starts the program, prints the time it started, and
# asks YOU to press Enter the moment the window is on the screen; it takes the time between the two. (Programs that are not
# terminal programs give a script nothing to wait on, and the window server is the page's own: the script cannot see its
# windows. AB_AUTO=1 tries to detect the first drawn frame by itself instead, from WAYLAND_DEBUG=1's protocol trace: an
# xdg_surface.ack_configure followed by a wl_surface.attach of a buffer; it gave nothing in the first tests, so it is off.)
# Your reaction time is in every number (about 0.2-0.4 s, the same for both versions); to cut it out keep the terminal and the
# new window both in view. After Enter the program is stopped and the next one starts after 3 seconds.
# Dolphin is started as `dolphin --new-window DIR` (a Dolphin that is already running would take the request and exit: the
# script refuses to run while one is), Kate as `kate -n` (a new instance), the rest by name. The native one is `nqs-run PROGRAM ...`.
# Output: a line per run and a summary table.
APPS=${1:-both}
RUNS=${2:-2}
DIR=${AB_DIR:-/}
LIMIT=${AB_LIMIT:-300}		# seconds AB_AUTO waits for a window before giving up
case $APPS in
both) APPS="dolphin systemsettings kate okular gwenview" ;;
dolphin | systemsettings | kate | kwrite | okular | gwenview | ark) ;;
*) echo "usage: native-ab.sh [dolphin|systemsettings|kate|kwrite|okular|gwenview|ark|both] [RUNS]" >&2; exit 2 ;;
esac
command -v nqs-run > /dev/null || { echo "native-ab: no nqs-run: open the page with ?dl=1" >&2; exit 1; }
[ -n "$WAYLAND_DISPLAY" ] || export WAYLAND_DISPLAY=wayland-0
[ -n "$XDG_RUNTIME_DIR" ] || export XDG_RUNTIME_DIR=/tmp/runtime-root
[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] || { echo "native-ab: no window server at $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" >&2; exit 1; }
for p in dolphin systemsettings kate kwrite okular gwenview ark ld-wasm; do
	if pgrep -x $p > /dev/null; then echo "native-ab: $p is running (ld-wasm: a native app): close it first, and run this in an emulated terminal (xterm, or Debian's Konsole), not a native Konsole" >&2; exit 1; fi
done

now() { date +%s.%N; }
shown() {
	awk '/xdg_surface@[0-9]+\.ack_configure/ { a = 1 } a && /wl_surface@[0-9]+\.attach\(wl_buffer@/ { f = 1; exit } END { exit !f }' "$1"
}
stop_all() {
	for p in dolphin systemsettings kate kwrite okular gwenview ark ld-wasm kioworker; do pkill -x $p 2> /dev/null; done
}
# one MODE APP N: starts the program, prints the seconds to the window (to stdout; prompts go to the terminal)
one() {
	local mode=$1 app=$2 n=$3 log=/tmp/ab-$2-$1-$3.log cmd t0 t1 pid args=
	case $app in
	dolphin) args="--new-window $DIR" ;;
	kate) args="-n" ;;
	esac
	if [ $mode = native ]; then cmd="nqs-run $app $args"; else cmd="$app $args"; fi
	: > $log
	t0=$(now)
	if [ -n "$AB_AUTO" ]; then
		WAYLAND_DEBUG=1 QT_QPA_PLATFORM=wayland $cmd > $log 2>&1 &
	else
		QT_QPA_PLATFORM=wayland $cmd > $log 2>&1 &
	fi
	pid=$!
	echo "  started $mode $app (run $n) at $(date +%T): press Enter when its window is on the screen" > /dev/tty
	if [ -n "$AB_AUTO" ]; then
		while kill -0 $pid 2> /dev/null && ! shown $log; do
			awk -v a="$(now)" -v b="$t0" -v l="$LIMIT" 'BEGIN { exit !(a - b > l) }' && break
			sleep 0.05
		done
		t1=$(now)
		shown $log || t1=
	else
		read -r _ < /dev/tty
		t1=$(now)
	fi
	stop_all
	wait $pid 2> /dev/null
	sleep 3
	if [ -n "$t1" ]; then awk -v a="$t1" -v b="$t0" 'BEGIN { printf "%.1f\n", a - b }'; else echo 0; fi
}

echo "app            run  emulated(s)  native(s)  ratio"
for app in $APPS; do
	for n in $(seq 1 $RUNS); do
		e=$(one emulated $app $n)
		v=$(one native $app $n)
		r=$(awk -v a="$e" -v b="$v" 'BEGIN { if (b > 0) printf "%.1fx", a / b; else print "-" }')
		printf '%-14s %3d  %10.1f  %9.1f  %s\n' $app $n $e $v "$r"
	done
done
echo "(the first launch of each also compiles the libraries it needs: a second visit after a reload is faster; the numbers include your reaction time)"
