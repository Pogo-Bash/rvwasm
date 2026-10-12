#!/bin/bash
# Apply Debian's patches (debian/patches/series) to each Qt module's source, once.
set -e
. "$(dirname "$0")/env.sh"
for m in "$@"; do
	S=$NQ_SRC/$m
	[ -e $S/.nq-debian-patched ] && continue
	if [ -f $S/debian/patches/series ]; then
		grep -v -E '^\s*(#|$)' $S/debian/patches/series | while read -r p rest; do
			patch -d $S -p1 -s -N < $S/debian/patches/$p || echo "debian patch $p failed in $m"
		done
	fi
	# Ours (patches/nativeqt-$m.diff), if any.
	[ -f $NQ_TREE/patches/nativeqt-$m.diff ] && patch -d $S -p1 -s -N < $NQ_TREE/patches/nativeqt-$m.diff
	touch $S/.nq-debian-patched
done
