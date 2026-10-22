#!/bin/sh
# Run inside the chroot (as a client of `desktop wayland`, which has set up the environment and made the menu cache): what the
# desktop's menu knows of the native programs. tests/wasmdyn/check-default.sh puts this file in the guest and runs it.
#   guest-menu-check.sh LABEL
echo "MENU-ENV-$1: XDG=$XDG_DATA_DIRS"
echo "MENU-ENV-$1: PATH=$PATH"
for f in /root/.cache/ksycoca6_*; do
	echo "MENU-CACHE-$1: $f"
	# (the cache has its strings as UTF-16: tr drops the NULs and the other control characters)
	tr -d '[:cntrl:]' < $f > /tmp/ksy.txt
	for n in Dolphin Konsole Kate KWrite KCalc Okular Gwenview Ark "System Settings"; do
		echo "MENU-NAME-$1: $n x$(grep -o "$n" /tmp/ksy.txt | wc -l)"
	done
	echo "MENU-NATIVE-LABEL-$1: $(grep -o '(native)' /tmp/ksy.txt | wc -l) entries say (native)"
	echo "MENU-NQSRUN-$1: $(grep -o 'nqs-run' /tmp/ksy.txt | wc -l) mentions of nqs-run"
done
echo "MENU-STAMPS-$1: menu=$(cat /root/.cache/rv-ksycoca-menu 2> /dev/null) nqs=$(cat /root/.cache/rv-ksycoca-nqs 2> /dev/null)"
# the ID -> file the menu resolves (menu test: every entry the menu has, with the file it came from)
kbuildsycoca6 --menutest 2> /dev/null | grep -i -E "dolphin|konsole|kate|kwrite|kcalc|okular|gwenview|ark\.|systemsettings" | head -40 | sed "s/^/MENU-ENTRY-$1: /"
for id in org.kde.dolphin.desktop org.kde.konsole.desktop org.kde.kate.desktop systemsettings.desktop org.kde.okular.desktop org.kde.ark.desktop; do
	for d in $(echo "$XDG_DATA_DIRS" | tr ':' ' '); do
		[ -f $d/applications/$id ] && { echo "MENU-FIRST-$1: $id from $d: $(grep -m1 '^Exec=' $d/applications/$id)"; break; }
	done
done
