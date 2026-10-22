#!/bin/bash
# gpuaddr-check: the GPU paths with every guest address above 2 GiB, then above 4 GiB (tools/gpu-nm.sh, the real executor).
#
# The bug it guards against: the GPU server, the executor and the rest turned an address of the machine's memory into
# a typed-array index with `>> 2` (or rounded it with `& ~4095`, or `| 0`): JavaScript's 32-bit integers. From 2 GiB
# on that gives a negative index (a RangeError, and the GPU server dies); from 4 GiB on it wraps to a LOW address, and
# the program reads and writes the wrong memory, usually the kernel's. A machine with mem=8192M (the page's default on a
# big computer) has such addresses once it has been used a while, with or without ?mmu=1.
#
#   gpu/tools/gpuaddr-check.sh [FLOOR_GIB...]      (default: 2.5 4.5), each with es2gears (GL through virgl/zink) and vkcube
#                                                  (Vulkan through venus); GPUADDR_PROGS="es2gears vkcube mpv" says which
#
# For each floor the machine runs at mem=8192M and, before the program, gpu/server/ballast.js takes all the free memory
# below the floor (and fills it with 0xA5), so that everything the machine hands out after it lies above: the GPU
# server's ring and backings, the window server's table, the clients' buffers. It passes when
#   - gpu-nm's own verdict is PASS (executor ready, ring moved, frames, window drawn, pixel probe),
#   - the GPU server's ring and the window's buffer are at or above the floor (the memory was really up there),
#   - the ballast's memory is intact afterwards (no write went to a wrapped address).
# Exit status: 0 when all pass, 1 otherwise. Output: one line per run, the details in build/gpuaddr/NAME.txt.
cd "$(dirname "$0")/../.."
FLOORS=${*:-2.5 4.5}
PROGS=${GPUADDR_PROGS:-"es2gears vkcube"}
mkdir -p build/gpuaddr
status=0
for floor in $FLOORS; do
	for prog in $PROGS; do
		name=$prog-$floor
		# (no program is started while the ballast takes the memory: with every block of 256 KiB and more taken, the machine cannot
		# exec anything for a second; the shell waits with builtins only, `read -t` on a pipe it holds open)
		pre='mkfifo /tmp/bf; exec 3<>/tmp/bf; rm -f /tmp/ballast.ready /tmp/ballast.failed; (HOSTNODE_HOME=/run/rvgpu RVGPU_LOAD=ballast.js RVGPU_BALLAST_GIB='$floor' /run/hostnode/hostnode > /tmp/ballast.log 2>&1 &); i=0; while [ ! -e /tmp/ballast.ready ] && [ ! -e /tmp/ballast.failed ] && [ $i -lt 120 ]; do read -t 1 -u 3 x; i=$((i+1)); done; cat /tmp/ballast.log;'
		post='touch /tmp/ballast.verify; j=0; while [ -e /tmp/ballast.verify ] && [ $j -lt 90 ]; do sleep 1; j=$((j+1)); done; grep -a "^ballast:" /tmp/ballast.log; grep -a "gpuserver: listening\|video decoding ring" /root/rvgpu.log; tail -4 /root/rvgpu.log; touch /tmp/ballast.stop;'
		GPU_NM_APPEND=mem=8192M GPU_NM_PRE="$pre" GPU_NM_POST="$post" tools/gpu-nm.sh -t ${GPUADDR_SECS:-25} $prog > build/gpuaddr/$name.txt 2>&1
		rc=$?
		cp build/gpu-nm/last.out build/gpuaddr/$name.out; cp build/gpu-nm/last.err build/gpuaddr/$name.err
		out=build/gpu-nm/last.out
		ring=$(grep -a -o 'listening on [^ ]* (ring at [0-9]*' $out | grep -o '[0-9]*$' | tail -1)
		vring=$(grep -a -o 'video decoding ring at [0-9]*' $out | grep -o '[0-9]*$' | tail -1)
		ready=$(grep -a 'ballast: ready' $out | tail -1)
		verdict=$(grep -a -E 'ballast: (intact|CLOBBERED)' $out | tail -1)
		buf=$(grep -a -o 'buffer [0-9]*' build/gpu-nm/last.err | awk '{print $2}' | sort -n | awk '$1 < 68719476736' | tail -1)
		ok=1
		why=
		[ $rc = 0 ] || { ok=0; why="$why gpu-nm FAIL (rc $rc);"; }
		[ -n "$ready" ] || { ok=0; why="$why no ballast;"; }
		if [ -n "$ring" ] && awk -v r="$ring" -v f="$floor" 'BEGIN{exit !(r >= f * 1073741824)}'; then :; else ok=0; why="$why ring ${ring:-none} below the floor or none;"; fi
		if [ -n "$vring" ] && ! awk -v r="$vring" -v f="$floor" 'BEGIN{exit !(r >= f * 1073741824)}'; then ok=0; why="$why video ring $vring below the floor;"; fi
		case "$verdict" in *intact*) ;; *) ok=0; why="$why ${verdict:-ballast not verified};";; esac
		[ $ok = 1 ] && r=PASS || { r=FAIL; status=1; }
		echo "gpuaddr $name: $r  floor ${floor} GiB  ring ${ring:-?}${vring:+  video ring $vring}  window buffer(max) ${buf:-?}  ${verdict#*ballast: }  ${why}"
	done
done
exit $status
