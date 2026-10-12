#!/bin/bash
# In Debian: run-ps.sh PROGRAM SECONDS [trace|fresh]: plasmashell (riscv64 /usr/bin/plasmashell or the native one) on
# wlserver for SECONDS, its output in /tmp/ps.log; with "trace", Qt's plugin and QML import tracing too. Prints
# what it loaded (plugins, QML modules), the Wayland globals it asked for and did not get, and the times.
prog=$1 secs=$2
export QT_QUICK_BACKEND=software QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu
# (what Plasma's platform theme, plasma-integration, would set: Breeze's look for Kirigami and Qt Quick Controls)
export QT_QUICK_CONTROLS_STYLE=org.kde.desktop
[ "$3" = trace ] && export QT_DEBUG_PLUGINS=1 QML_IMPORT_TRACE=1 WAYLAND_DEBUG=client
# fresh: an empty configuration for plasmashell (its own files only), so that it lays out Plasma's default
# desktop (the look-and-feel package's layout script: a desktop and a bottom panel) instead of the disk's
# appletsrc, which the page's desktop wrote with a desktop containment only.
if [ "$3" = fresh ]; then
	mkdir -p /tmp/ps-config && rm -f /tmp/ps-config/plasma-org.kde.plasma.desktop-appletsrc /tmp/ps-config/plasmashellrc
	export PS_CONFIG=/tmp/ps-config
fi
desktop wayland sleep 900 > /tmp/desktop.log 2>&1 &
export XDG_RUNTIME_DIR=/tmp/runtime-root WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland
while [ ! -S $XDG_RUNTIME_DIR/wayland-0 ]; do sleep 0.5; done
# The desktop's own session bus (tools/x/desktop starts it; wlserver is org.kde.KWin, org.kde.plasmashell's
# wallpaper part, the notifications on it), and Debian's kglobalacceld on it (KWin hosts it in a Plasma
# session; nothing does here).
while [ ! -S $XDG_RUNTIME_DIR/bus ]; do sleep 0.5; done
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
/usr/lib/riscv64-linux-gnu/libexec/kglobalacceld > /tmp/kglobalacceld.log 2>&1 &
t0=$(date +%s%3N)
echo "run-ps: launch at $t0: $prog"
if [ -n "$PS_CONFIG" ]; then XDG_CONFIG_HOME=$PS_CONFIG "$prog" > /tmp/ps.log 2>&1 & else "$prog" > /tmp/ps.log 2>&1 & fi
pid=$!
# CPU over the last 30 s (idle with its desktop up), and its mappings, read from outside before it is ended
sleep $(( secs - 30 ))
read -r st0 < /proc/$pid/stat; f0=($st0)
sleep 30
read -r st1 < /proc/$pid/stat; f1=($st1)
m=0
while read -r range rest; do
	[[ $range == [0-9a-f]*-[0-9a-f]* ]] || continue
	a=${range%-*} b=${range#*-}
	m=$(( m + (16#$b - 16#$a) / 1024 ))
done < /proc/$pid/maps
echo "run-ps: last 30 s: $(( f1[13] + f1[14] - f0[13] - f0[14] )) ticks of CPU (100 a second); mappings $(( m / 1024 )) MiB; total CPU $(( f1[13] + f1[14] )) ticks"
kill $pid 2> /dev/null; sleep 1; kill -9 $pid 2> /dev/null
echo "run-ps: $(wc -l < /tmp/ps.log) lines of output"
[ $(wc -l < /tmp/ps.log) -lt 400 ] && { echo "--- whole output"; cut -c1-300 /tmp/ps.log; }
echo "--- plugins loaded"
grep -i "loaded library\|loadPlugin\|succeeded on" /tmp/ps.log | grep -o '"/[^"]*\.so[^"]*"' | sed 's|.*/||; s|"||g' | sort -u | tr '\n' ' '
echo
echo "--- QML modules with plugins"
grep "qt.qml.import" /tmp/ps.log | grep -io 'plugin "[^"]*"\|module "[^"]*"\|importExtension.*\|qmldir file [^ ]*\|loaded plugin[^,]*' | sed 's|/usr/lib/riscv64-linux-gnu/qt6/qml/||' | sort -u | head -200
echo "--- QML import URIs"
grep "qt.qml.import" /tmp/ps.log | grep -o 'import "[^"]*"' | sort -u | tr '\n' ' '
echo
echo "--- wayland globals bound"
grep -o 'wl_registry[#@][0-9]*\.bind([0-9]*, "[a-z_0-9]*"' /tmp/ps.log | grep -o '"[a-z_0-9]*"' | sort -u | tr '\n' ' '
echo
echo "--- wayland globals offered"
grep -o 'wl_registry[#@][0-9]*\.global([0-9]*, "[a-z_0-9]*"' /tmp/ps.log | grep -o '"[a-z_0-9]*"' | sort -u | tr '\n' ' '
echo
echo "--- times"
grep -m 3 "wl_surface[#@][0-9]*\.commit" /tmp/ps.log | head -3
echo "--- warnings (first 40 distinct)"
grep -v "^\[\|^ \|^{\|^}\|wl_\|xdg_\|zwp_\|wp_\|qt.qml.import\|QFactoryLoader\|qt.core\|Got keys\|Found metadata\|\"" /tmp/ps.log | sort | uniq -c | sort -rn | head -40 | cut -c1-200
