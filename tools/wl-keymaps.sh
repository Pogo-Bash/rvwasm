#!/bin/bash
# The keymaps the window server offers (wlserver/keymaps/): one compiled XKB
# keymap per layout, as `xkbcli compile-keymap` makes them from the Debian
# root's xkb-data, and an index the server and the page read.
#
# Run in the build VM, with an rv to run the riscv64 xkbcli under and the
# libxkbcommon-tools package unpacked somewhere:
#   tools/wl-keymaps.sh RV ROOT XKBCLI OUT_DIR
# e.g.
#   tools/wl-keymaps.sh ~/rvwasm/build-rv/rv ~/rvwasm/debian-root-kde \
#     ~/wlwork/xkbt/usr/libexec/xkbcommon/xkbcli-compile-keymap wlserver/keymaps
#
# A layout is added by a line below: the file's name, xkb's layout and
# variant, and what the page calls it.
set -e
RV=$1 ROOT=$2 XKBCLI=$3 OUT=$4
[ -x "$RV" ] && [ -d "$ROOT" ] && [ -e "$XKBCLI" ] && [ -n "$OUT" ] || {
	echo "usage: tools/wl-keymaps.sh RV ROOT XKBCLI OUT_DIR" >&2
	exit 2
}
mkdir -p "$OUT"
rm -f "$OUT"/*.xkb "$OUT/index.txt"
while IFS='|' read -r name layout variant description; do
	[ -z "$name" ] && continue
	args=(--layout "$layout")
	[ -n "$variant" ] && args+=(--variant "$variant")
	/usr/bin/env -i PATH="$ROOT/usr/bin" RV_SYSROOT="$ROOT" "$RV" "$XKBCLI" --rules evdev --model pc105 "${args[@]}" \
		> "$OUT/$name.xkb"
	[ -s "$OUT/$name.xkb" ] || { echo "no keymap for $name" >&2; exit 1; }
	printf '%s\t%s\n' "$name" "$description" >> "$OUT/index.txt"
done <<'END'
us|us||English (US)
us-intl|us|intl|English (US, intl., with dead keys)
us-dvorak|us|dvorak|English (Dvorak)
us-colemak|us|colemak|English (Colemak)
gb|gb||English (UK)
de|de||German
de-nodeadkeys|de|nodeadkeys|German (no dead keys)
ch|ch||German (Switzerland)
ch-fr|ch|fr|French (Switzerland)
fr|fr||French
be|be||Belgian
ca|ca||French (Canada)
es|es||Spanish
latam|latam||Spanish (Latin American)
it|it||Italian
pt|pt||Portuguese
br|br||Portuguese (Brazil)
nl|nl||Dutch
se|se||Swedish
no|no||Norwegian
dk|dk||Danish
fi|fi||Finnish
pl|pl||Polish
cz|cz||Czech
hu|hu||Hungarian
tr|tr||Turkish
gr|gr||Greek
ru|ru||Russian
ua|ua||Ukrainian
jp|jp||Japanese
END
ls -la "$OUT" | head -40
