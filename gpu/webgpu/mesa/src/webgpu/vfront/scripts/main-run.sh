#!/bin/bash
# (In the VM, outside the chroot) vfront-vtest from this track's Mesa-main build ($B/native), with the guest
# side (virpipe, piglit, glmark2, es2gears: the trixie chroot, t6-chroot.sh) talking to it over a socket in
# /t6/run. The server runs on the VM's own libraries, so it is not started inside the chroot.
#   main-run.sh [--vk lvp|wgvk] NAME 'CHROOT CMD'      the command gets VTEST_SOCKET_NAME=/t6/run/NAME.sock
# --vk lvp (default): zink on lavapipe (the integration build's). --vk wgvk: zink on wgvk on Dawn on lavapipe.
# VFRONT_VTEST_ARGS go to the server (-v, --capture FILE). Server log: ~/rvwasm/gpu-t6/log/NAME-server.log
vk=lvp
[ "$1" = --vk ] && { vk=$2; shift 2; }
name=$1; cmd=$2
B=$HOME/rvwasm/webgpu/b/${VFRONT_BUILD:-gpu-t6}
LVP_ICD=$HOME/rvwasm/webgpu/b/webgpu/native/src/gallium/targets/lavapipe/lvp_devenv_icd.aarch64.json
WGVK_ICD=$B/native/src/webgpu/vulkan/wgvk_devenv_icd.aarch64.json
R=$HOME/rvwasm/gpu-t6/run
mkdir -p $R $HOME/rvwasm/gpu-t6/log
sock=$R/$name.sock
rm -f $sock
if [ $vk = wgvk ]; then
	export VK_DRIVER_FILES=$WGVK_ICD WGVK_DAWN_DRIVER_FILES=$LVP_ICD WGVK_DEVICE_TYPE=cpu
else
	export VK_DRIVER_FILES=$LVP_ICD
fi
log=$HOME/rvwasm/gpu-t6/log/$name-server.log
# (a private copy: a rebuild during a long run must not change the server that restarts)
bin=$R/$name-vfront-vtest
rm -f $bin
cp $B/native/src/webgpu/vfront/vfront-vtest $bin
: > $log
start() {
	rm -f $sock
	# (GDB=1: under gdb, which prints a backtrace into the log when it dies)
	local pre=()
	[ "$GDB" = 1 ] && pre=(gdb -q -batch -ex "handle SIGPIPE nostop noprint" -ex run -ex "bt 25" --args)
	LIBGL_ALWAYS_SOFTWARE=1 "${pre[@]}" $bin --socket $sock $VFRONT_VTEST_ARGS >> $log 2>&1 &
	spid=$!
	for i in $(seq 900); do [ -S $sock ] && break; kill -0 $spid 2>/dev/null || break; sleep 0.1; done
	[ -S $sock ]
}
start || { echo "main-run: server did not start"; tail -20 $log; exit 1; }
# A server that dies is restarted (RESTART=0: not), so one crash does not fail every test after it; each
# death is logged with the time.
(
	while sleep 0.5; do
		# (the server leaks: past MAXRSS_MB it is restarted before the VM runs out of memory)
		rss=$(awk '/VmRSS/ {print int($2 / 1024)}' /proc/$spid/status 2>/dev/null)
		if [ "${rss:-0}" -gt "${MAXRSS_MB:-7000}" ]; then
			echo "=== main-run: server at $rss MB, restarting" >> $log
			kill $spid 2>/dev/null
			sleep 1
		fi
		kill -0 $spid 2>/dev/null && continue
		[ "${RESTART:-1}" = 1 ] || break
		echo "=== main-run: server died at $(date +%T), restarting" >> $log
		start || break
	done
) &
wpid=$!
bash $(dirname "$0")/t6-chroot.sh "export VTEST_SOCKET_NAME=/t6/run/$name.sock; $cmd"
rc=$?
kill $wpid 2>/dev/null
pkill -f "$bin --socket $sock" 2>/dev/null
echo "main-run: server deaths: $(grep -c '^=== main-run: server died' $log)"
exit $rc
