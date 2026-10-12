# Benchmarks for comparing ways of running riscv64 Linux programs: the same
# static riscv64 binaries and the same commands everywhere, timed from inside
# with the system's own clock.
#
#   PATH=<riscv64 BusyBox applets> COREMARK=<path> BUSYBOX=<path> sh bench.sh
#
# Run it in a writable directory with ~20 MiB free. Each result is one line,
#   BENCH <name> <milliseconds> <check>
# where <check> (a hash or a count) must be the same on every system.

set -u
: "${COREMARK:?path to the riscv64 coremark}"
: "${BUSYBOX:?path to the riscv64 busybox binary}"

# Centiseconds since boot. (/proc/uptime is everywhere; BusyBox's date has no sub-second part.)
now() {
	read up rest < /proc/uptime
	# (Not just the digits run together: "0.95" would give 095, which is not a number in octal.)
	echo "$(( ${up%.*} * 100 + 1${up#*.} - 100 ))"
}

# run NAME COMMAND...: time a command, report its output's first word as the check.
run() {
	name=$1
	shift
	start=$(now)
	check=$("$@" 2>&1 | tail -n 1)
	end=$(now)
	echo "BENCH $name $(( (end - start) * 10 )) ${check%% *}"
}

echo "== bench start"
dd if=/dev/zero of=bench.big bs=1M count=16 2> /dev/null

# CPU-bound, one process: CoreMark reports its own time for the timed part.
out=$("$COREMARK" 0 0 0x66 2000)
echo "BENCH coremark_ticks $(echo "$out" | sed -n 's/^Total ticks *: *//p') $(echo "$out" | sed -n 's/^\[0\]crcfinal *: *//p')"

run sha256_16M sha256sum bench.big
run md5_16M md5sum bench.big
run gzip sh -c "gzip -c $BUSYBOX | md5sum"
run sort sh -c "seq 1 200000 | sort -rn | md5sum"
run awk sh -c "seq 1 300000 | awk '{ s += \$1 * \$1 } END { printf \"%d\n\", s }'"

# Process creation: 200 fork+exec pairs, and a pipeline of small tools.
run spawn200 sh -c "i=0; while [ \$i -lt 200 ]; do $BUSYBOX true; i=\$((i + 1)); done; echo \$i"
run pipeline sh -c "i=0; while [ \$i -lt 20 ]; do seq 1 50 | grep 7 | sort -r | head -n 3 | wc -l > /dev/null; i=\$((i + 1)); done; echo \$i"

rm -f bench.big
echo "== bench done"
