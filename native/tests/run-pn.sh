#!/bin/bash
# In Debian: run-pn.sh PROGRAM SECONDS: Plasma's own shell on wlserver (`desktop wayland plasma-native`, which runs
# the first plasmashell on the PATH: PROGRAM is put in /usr/local/bin for that) with Plasma's default layout
# (no appletsrc: a desktop and the bottom panel), Konsole started after SECONDS/2, and the shell's CPU and
# mappings read from outside at the end.
prog=$1 secs=$2
[ "$prog" != /usr/bin/plasmashell ] && cp "$prog" /usr/local/bin/plasmashell
[ "$prog" = /usr/bin/plasmashell ] && rm -f /usr/local/bin/plasmashell
rm -f /root/.config/plasma-org.kde.plasma.desktop-appletsrc /root/.config/plasmashellrc
export QT_QUICK_BACKEND=software QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu QT_QUICK_CONTROLS_STYLE=org.kde.desktop
echo "run-pn: launch at $(date +%s%3N): $prog"
desktop wayland plasma-native > /tmp/pn.log 2>&1 &
sleep 5
for i in $(seq 1 60); do pid=$(pgrep -n -x plasmashell); [ -n "$pid" ] && break; sleep 1; done
echo "run-pn: plasmashell pid $pid"
sleep $(( secs / 2 ))
echo "run-pn: konsole at $(date +%s%3N)"
desktop wayland konsole > /tmp/konsole.log 2>&1 &
sleep $(( secs / 2 - 30 ))
read -r st0 < /proc/$pid/stat; f0=($st0)
sleep 30
read -r st1 < /proc/$pid/stat; f1=($st1)
m=0
while read -r range rest; do
	[[ $range == [0-9a-f]*-[0-9a-f]* ]] || continue
	a=${range%-*} b=${range#*-}
	m=$(( m + (16#$b - 16#$a) / 1024 ))
done < /proc/$pid/maps
echo "run-pn: last 30 s: $(( f1[13] + f1[14] - f0[13] - f0[14] )) ticks of CPU; mappings $(( m / 1024 )) MiB; total CPU $(( f1[13] + f1[14] )) ticks"
echo "--- plasmashell's output (distinct lines)"
grep -v "^qt.core.plugin.loader: Cannot load\|Found plugin with old IID" /tmp/pn.log | sort | uniq -c | sort -rn | head -40 | cut -c1-1500
