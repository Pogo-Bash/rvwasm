#!/bin/bash
# (In the VM) each command line of FILE (piglit binaries, as the chroot sees them) against its own fresh
# main-build server (main-run.sh): one line per test with its exit status, its last output line and whether
# the server survived it.   main-each.sh FILE [--vk lvp|wgvk]
S=$(dirname "$0")
while IFS= read -r t; do
	[ -z "$t" ] && continue
	out=$(RESTART=0 bash $S/main-run.sh $2 $3 each "SERVER=external T=${T:-60} LINES_OUT=1 SLINES=0 bash /t6/vfront/scripts/pg-one.sh '$t'" 2>&1)
	died=$(echo "$out" | grep -q "server deaths: [1-9]\|SERVER DIED" && echo " SERVER-DIED")
	[ -z "$died" ] && ! pgrep -f "each-vfront-vtest" > /dev/null && died=""
	crash=$(grep -m1 "Assertion\|signal [0-9]* while" ~/rvwasm/gpu-t6/log/each-server.log | cut -c1-160)
	echo "$(echo "$out" | grep -m1 '^test rc' | sed 's/test //')$died | $(echo "$out" | grep -v '^test rc\|main-run' | head -1 | cut -c1-100) | ${t#/usr/lib/aarch64-linux-gnu/piglit/bin/} ${crash:+| $crash}"
done < "$1"
