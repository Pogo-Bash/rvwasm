#!/bin/bash
# Run a command that uses what there is only one of: the test browser, and
# with it the staged build that the dev server hands to that browser.
#
#   tools/with-browser.sh WHO COMMAND...
#
# Run on the Mac. One at a time: a second caller waits here until the first
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
waited=0
until mkdir "$LOCK" 2> /dev/null; do
	holder="$(cat "$LOCK/pid" 2> /dev/null)"
	# Left behind by something that is gone?
	if [ -n "$holder" ] && ! kill -0 "$holder" 2> /dev/null; then
		rm -rf "$LOCK"
		continue
	fi
	if [ $((waited % 300)) -eq 0 ]; then
		echo "with-browser: $who waits for $(cat "$LOCK/who" 2> /dev/null) (pid $holder), ${waited}s so far" >&2
	fi
	sleep 15
	waited=$((waited + 15))
done
echo $$ > "$LOCK/pid"
echo "$who" > "$LOCK/who"

# A test browser whose driver is gone (killed, or ended by a signal it did not
# handle) stays behind, re-parented to launchd, each with a machine of its own
# of several GB: twice in one evening two of them sat next to the live one.
# Nobody else may be running one, so any such browser is an orphan: end it,
# before this run and after it.
orphans() {
	local p
	for p in $(pgrep -f "MacOS/Google Chrome for Testing --headless"); do
		if [ "$(ps -o ppid= -p "$p" | tr -d ' ')" = 1 ]; then
			echo "with-browser: $who ends a test browser left behind (pid $p)" >&2
			pkill -9 -P "$p"
			kill -9 "$p" 2> /dev/null
		fi
	done
	# Browsers also leave their profiles: those of browsers that are gone.
	for d in "${TMPDIR:-/tmp}"/rvbench-* "${TMPDIR:-/tmp}"/rvdrive-*; do
		[ -d "$d" ] && ! pgrep -f "user-data-dir=$d" > /dev/null && rm -rf "$d"
	done
	return 0
}
orphans
trap 'orphans; rm -rf "$LOCK"' EXIT
trap 'exit 130' INT TERM

RV_BROWSER=1 "$@"
