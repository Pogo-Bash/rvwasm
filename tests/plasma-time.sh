# How long does the Plasma desktop take to come up? Starts the session and
# says, every five seconds, how many windows plasmashell has; the panel and
# the desktop are there when there are several.
#
#   Linux/Wasm:  cp /rv/plasma-time.sh /debian/root/ && debian 'bash /root/plasma-time.sh'

(desktop plasma &)
export DISPLAY=:0
for i in $(seq 1 60); do
	sleep 5
	n=$(xwininfo -root -tree 2> /dev/null | grep -c -i plasmashell)
	p=$(ps -e -o comm | grep -c "plasmashell\|kwin_x11\|kded6\|ksmserver")
	echo "$((i * 5)) s: $n windows of plasmashell, $p of the session's four main programs running"
	[ "$n" -ge 4 ] && break
done
grep MemAvailable /proc/meminfo
