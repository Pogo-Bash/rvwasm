#!/bin/sh
# Inside the chroot, as a client of `desktop wayland` (tests/wasmdyn/check-default.sh SCENARIO=mime): open a PDF, a PNG and a ZIP the
# way Dolphin does (KIO's OpenUrlJob through the desktop's mime associations: kde-open) and say which program each started.
mkdir -p /tmp/m
printf '%%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>>endobj\ntrailer<</Root 1 0 R>>\n%%%%EOF\n' > /tmp/m/t.pdf
printf 'PK\005\006\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0' > /tmp/m/t.zip
cp $(find /usr/share/icons /usr/share/pixmaps -name '*.png' 2> /dev/null | head -1) /tmp/m/t.png
ls -la /tmp/m
for f in t.pdf t.png t.zip; do
	echo "MIME-OPEN: $f $(kmimetypefinder /tmp/m/$f 2> /dev/null)"
	kde-open /tmp/m/$f > /tmp/m/$f.out 2>&1 &
	sleep ${MIME_WAIT:-100}
	echo "MIME-PS-$f: $(ps | grep -E 'okular|gwenview|ark' | grep -v grep | head -3 | tr '\n' '|')"
	cat /tmp/m/$f.out | head -3
	pkill -x ld-wasm; pkill -x okular; pkill -x gwenview; pkill -x ark
	sleep 3
done
echo MIME-DONE
