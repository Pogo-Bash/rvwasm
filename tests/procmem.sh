# What does one process of the emulator have mapped on the host? Starts a
# program that does nothing and lists its mappings by size (Linux/Wasm lists
# every piece of memory a process has in /proc/PID/maps).
#
#   Linux/Wasm:  cp /rv/procmem.sh /debian/root/ && debian 'bash /root/procmem.sh [PROGRAM ARGS...]'

[ $# -gt 0 ] || set -- sleep 30
"$@" > /dev/null 2>&1 &
pid=$!
sleep 3
total=0
while read -r range perms rest; do
	a=${range%-*}
	b=${range#*-}
	kib=$(( (0x$b - 0x$a) / 1024 ))
	total=$((total + kib))
	echo "$kib KiB $perms ${rest##* }"
done < /proc/$pid/maps | sort -rn | head -${TOP:-22}
echo "mappings: $(wc -l < /proc/$pid/maps)"
grep -i "^vm\|^rss" /proc/$pid/status
kill $pid 2> /dev/null
