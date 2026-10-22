#!/bin/bash
# Run a command that uses what there is only one of: the test browser, and
# with it the staged build that the dev server hands to that browser.
#
#   tools/with-browser.sh WHO COMMAND...
#
# Run on the Mac. One at a time (a second slot opens when memory is plentiful, see below): a second caller waits here until the first
# is done (a machine in a browser is 4 to 9 GB, and two of them next to the
# owner's own work have filled this computer's swap). Staging belongs inside
# as well (tools/stage.sh in the VM, then the browser run that tests it):
# what is staged is what every browser gets, so two sessions that stage and
# test in turn would test each other's builds.
#
# WHO is a word for the log. The command sees RV_BROWSER=1, which
# tools/bench-browser.js and tools/desktop-drive.js insist on.

LOCK=/tmp/rv-browser.lock
who="$1"
shift
if [ -z "$who" ] || [ $# -eq 0 ]; then
	echo "usage: $0 WHO COMMAND..." >&2
	exit 2
fi

# The orchestrator can pause all browser use (the owner is testing): wait while the pause file exists.
PAUSE=/tmp/rv-browser.paused
while [ -e "$PAUSE" ]; do
	echo "with-browser: $who waits: browser use is paused ($(cat "$PAUSE" 2> /dev/null))" >&2
	sleep 60
done
# Two slots (owner, 2026-10-14): the second opens only while more than 55% of memory is free, so two browsers never
# squeeze the owner's own work. RV_BROWSER_EXCLUSIVE=1 (staging, anything that changes what every browser gets) takes both.
LOCK2=/tmp/rv-browser.lock2
free_pct() { memory_pressure 2> /dev/null | awk '/free percentage/{gsub("%","",$5); print $5}'; }
stale() { local d=$1 h; h="$(cat "$d/pid" 2> /dev/null)"; [ -d "$d" ] && [ -n "$h" ] && ! kill -0 "$h" 2> /dev/null && rm -rf "$d"; return 0; }
take() { mkdir "$1" 2> /dev/null && echo $$ > "$1/pid" && echo "$who" > "$1/who"; }
# First come, first served (2026-10-15: with nine agents polling every 15 s, one waited 65 min while others won in
# turn). Each waiter takes a ticket in QDIR; only the oldest live tickets may take a slot: the first may take either
# slot, the second only when two slots are open (the second slot's rules below). Tickets of waiters that are gone
# are dropped.
QDIR=/tmp/rv-browser.queue
mkdir -p "$QDIR"
TICKET="$QDIR/$$"
mkdir "$TICKET" 2> /dev/null; echo "$who" > "$TICKET/who"
queue_pos() {
	local d n=0 me=0
	for d in $(ls -1tr "$QDIR" 2> /dev/null); do
		kill -0 "$d" 2> /dev/null || { rm -rf "$QDIR/$d"; continue; }
		n=$((n + 1))
		[ "$d" = "$$" ] && { me=$n; break; }
	done
	echo "$me"
}
waited=0
HELD=
while :; do
	stale "$LOCK"; stale "$LOCK2"
	pos=$(queue_pos)
	second_ok=
	[ ! -e /tmp/rv-lowpower ] && [ "$(free_pct)" -gt 55 ] 2> /dev/null && second_ok=1
	if [ -n "$RV_BROWSER_EXCLUSIVE" ]; then
		if [ "$pos" = 1 ] && take "$LOCK"; then
			if take "$LOCK2"; then HELD="$LOCK $LOCK2"; break; fi
			rm -rf "$LOCK"
		fi
	elif [ "$pos" = 1 ]; then
		if take "$LOCK"; then HELD="$LOCK"; break; fi
		# Low power (/tmp/rv-lowpower exists): one browser only.
		if [ -n "$second_ok" ] && take "$LOCK2"; then HELD="$LOCK2"; break; fi
	elif [ "$pos" = 2 ] && [ -n "$second_ok" ]; then
		if take "$LOCK2"; then HELD="$LOCK2"; break; fi
	fi
	if [ $((waited % 300)) -eq 0 ]; then
		echo "with-browser: $who waits (place $pos in line) for $(cat "$LOCK/who" 2> /dev/null) / $(cat "$LOCK2/who" 2> /dev/null), ${waited}s so far" >&2
	fi
	sleep 15
	waited=$((waited + 15))
done
rm -rf "$TICKET"

# A test browser whose driver is gone (killed, or ended by a signal it did not
# handle) stays behind, re-parented to launchd, each with a machine of its own
# of several GB: twice in one evening two of them sat next to the live one.
# Nobody else may be running one, so any such browser is an orphan: end it,
# before this run and after it.
orphans() {
	local p
	# (Firefox test browsers too: the test copy is also the owner's everyday Firefox, so only HEADLESS ones are touched.)
	for p in $(pgrep -f "MacOS/Google Chrome for Testing --headless") $(pgrep -f "MacOS/firefox --headless"); do
		if [ "$(ps -o ppid= -p "$p" | tr -d ' ')" = 1 ]; then
			echo "with-browser: $who ends a test browser left behind (pid $p)" >&2
			pkill -9 -P "$p"
			kill -9 "$p" 2> /dev/null
		fi
	done
	# Browsers also leave their profiles: those of browsers that are gone. Each holds the machine's disk cache (~3 GB):
	# rvff-* (Firefox, native-firefox.js) and rvnat-* (native-chrome.js) once filled 283 GB of the Mac's disk.
	for d in "${TMPDIR:-/tmp}"/rvbench-* "${TMPDIR:-/tmp}"/rvdrive-* "${TMPDIR:-/tmp}"/rvff-* "${TMPDIR:-/tmp}"/rvnat-*; do
		[ -d "$d" ] && ! pgrep -f "$d" > /dev/null && rm -rf "$d"
	done
	return 0
}
orphans
trap 'orphans; rm -rf $HELD "$TICKET"' EXIT
trap 'exit 130' INT TERM

RV_BROWSER=1 "$@"
