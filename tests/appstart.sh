#!/bin/bash
# How long programs take to start, each run ROUNDS times (default 2) in a row: the first run is cold (unless the
# store of translated code has it: rv/jitcache.c), the later ones find what the first translated. Run inside the
# Debian system (`debian "bash /rv/appstart.sh [ROUNDS] [NAME...]"`); prints "NAME round SECONDS rc".
# Without a display: Qt programs draw offscreen (QT_QPA_PLATFORM=offscreen), the browsers run headless.
#   python    python3 importing json
#   konsole   Konsole with a session that runs `true`: the window, the profile, the terminal, the shell; it
#             ends when the session does
#   dolphin, systemsettings, plasmashell   --version: the program and its libraries loaded and linked, Qt started
#   firefox   firefox --headless --screenshot of a local page: the browser started, a page drawn
#   chromium  chromium --headless --dump-dom of a local page
ROUNDS=${1:-2}
shift
NAMES=${*:-python konsole dolphin systemsettings plasmashell firefox chromium}
export QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR=/tmp/appstart-run
mkdir -p $XDG_RUNTIME_DIR /tmp/appstart
chmod 700 $XDG_RUNTIME_DIR
echo '<p>hello</p>' > /tmp/appstart/page.html

run() {
	case $1 in
	python) python3 -c 'import json' ;;
	konsole) konsole -e true ;;
	dolphin|systemsettings|plasmashell) $1 --version ;;
	firefox) firefox --headless --screenshot /tmp/appstart/ff.png file:///tmp/appstart/page.html ;;
	chromium) chromium --headless --no-sandbox --disable-gpu --dump-dom file:///tmp/appstart/page.html ;;
	*) echo "no such: $1"; return 2 ;;
	esac
}

for name in $NAMES; do
	for round in $(seq 1 $ROUNDS); do
		start=$(date +%s.%N)
		run $name > /tmp/appstart/$name.out 2>&1
		rc=$?
		end=$(date +%s.%N)
		awk -v n=$name -v r=$round -v s=$start -v e=$end -v rc=$rc 'BEGIN { printf "%s %s %.2f %s\n", n, r, e - s, rc }'
	done
done
