#!/bin/bash
# A program started ROUNDS times in a row (default 2) inside Debian on the machine, each round with its own
# RV_JIT_LOG, for what the store of translated code and the page's shared batches (tools/patch-runtime.py, 29)
# save the second time. Run it under node-machine with tools/cpu-hook.js, which reads the "@@S" and "@@E"
# markers printed here and says the wall and CPU time (all threads) and the cores used between them:
#   node --experimental-wasm-memory64 tools/node-machine.js ... --hook tools/cpu-hook.js --disk daily \
#     'cp /rv/jitlaunch.sh /debian/root/; debian "bash /root/jitlaunch.sh"'
#   PROG=command T=seconds ROUNDS=n are read from the environment (T: a program that does not end by itself is killed).
#   Per round: "round N: rc, seconds, regions, from the store, ..., make ms (store), compile ms (store)".
# Run a program ROUNDS times (default 2), each with its own RV_JIT_LOG; prints the seconds and, per round, the
# regions made, from the store, ms making (store), ms the host took to compile. Markers for the CPU hook.
# PROG: the command (default `konsole -e true`); T: seconds to give it (a program that does not end itself is killed).
export QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR=/tmp/js-run QT_QUICK_BACKEND=software HOME=/root
mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
for i in $(seq 1 ${ROUNDS:-2}); do
	echo "@@S r$i"
	s=$(date +%s.%N)
	if [ -n "$T" ]; then
		RV_JIT_LOG=/tmp/jl$i timeout -s KILL $T ${PROG:-konsole -e true} > /tmp/launch$i.out 2>&1
	else
		RV_JIT_LOG=/tmp/jl$i ${PROG:-konsole -e true} > /tmp/launch$i.out 2>&1
	fi
	rc=$?
	[ -n "$LOGDUMP" ] && sed "s/^/LOG$i /" /tmp/jl$i
	e=$(date +%s.%N)
	echo "@@E r$i"
	awk -v s=$s -v e=$e -v i=$i -v rc=$rc '$1 == "R" { n++; c += $15; made += $11; madec += ($15 ? $11 : 0); comp += $12; compc += ($15 ? $12 : 0); same += $14; b += $16; f += $17; g += $18; w += $19 }
		END { printf "round %d: rc %d, %.2f s, %d regions, %d from the store, %d same bytes as before in the process; make %.0f ms (store %.0f), compile %.0f ms (store %.0f); make is build %.0f, store %.0f, function %.0f, module %.0f ms\n",
		      i, rc, e - s, n, c, same, made / 1e6, madec / 1e6, comp / 1e6, compc / 1e6, b / 1e6, f / 1e6, g / 1e6, w / 1e6 }' /tmp/jl$i
	# (RV_JIT_CACHE_VERIFY=1 in the environment: rv says when a function from the store is not the one made now.)
	if grep -q "is not the one made now" /tmp/launch$i.out; then
		echo "round $i: $(grep -c 'is not the one made now' /tmp/launch$i.out) functions from the store differ from the ones made now; the first:"
		grep -m1 'is not the one made now' /tmp/launch$i.out
	fi
	grep -h -i "null function\|signal handler crash\|null page" /tmp/launch$i.out | head -3
done
