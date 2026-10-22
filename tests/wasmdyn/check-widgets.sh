#!/bin/bash
# qtwidgets-hello on the shared Qt (run-widgets.sh) with the library manifest preloaded: the window is up and shown by
# wl-dump, the button's signal ran its slot, every library was compiled ahead and by no Worker, the platform plugin and
# xdg-shell (dlopen'ed with threads running) were taken from the page's. Prints a line per check and "widgets: N failed".
cd "$(dirname "$0")/../.."
B=build/wasmdyn
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1 (got $2, wanted $3)"; fails=$((fails + 1)); fi; }
tests/wasmdyn/run-widgets.sh 6 ld > $B/widgets-check.log 2>&1
has() { grep -q -F -- "$1" $B/widgets-check.log && echo 1 || echo 0; }
check "platform wayland" "$(has 'platform wayland')" 1
check "the window was shown" "$(has 'qtwidgets: shown')" 1
check "the button's slot ran (clicked 1)" "$(has 'qtwidgets: clicked 1')" 1
check "program exited 0" "$(has 'PROGRAM-RC=0')" 1
check "platform plugin dlopen'ed" "$(has 'dlopen /usr/lib/qt6/plugins/platforms/libqwayland-generic.so')" 1
check "xdg-shell dlopen'ed" "$(has 'dlopen /usr/lib/qt6/plugins/wayland-shell-integration/libxdg-shell.so')" 1
check "a screenshot was written" "$([ -s $B/shots/screen.png ] && echo 1 || echo 0)" 1
python3 tests/wasmdyn/dl-summary.py $B/qtw-dl/libs.json < $B/widgets-check.log | tail -1
check "no library compiled by a Worker" "$(python3 tests/wasmdyn/dl-summary.py $B/qtw-dl/libs.json < $B/widgets-check.log | tail -1 | sed 's/.*Worker: \([0-9]*\).*/\1/')" 0
echo "widgets: $fails failed"
exit $fails
