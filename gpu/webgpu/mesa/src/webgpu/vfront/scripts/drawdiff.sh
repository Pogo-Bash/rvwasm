#!/bin/bash
# In the chroot: for readback N of a capture, replay it on zink and on softpipe with a framebuffer dump after
# every draw of that frame, and report the first draw whose result differs, with the commands before it and
# the TGSI of the shaders it uses.   drawdiff.sh CAPTURE N
cap=$1; n=$2
R=/t6/build-native/src/gallium/frontends/vfront/vfront-replay
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
last=$(LIBGL_ALWAYS_SOFTWARE=1 VFRONT_REPLAY_TRACE=1 $R $cap 2>/dev/null | awk -v n=$n '$2==n {print $NF}')
first=$(LIBGL_ALWAYS_SOFTWARE=1 VFRONT_REPLAY_TRACE=1 $R $cap 2>/dev/null | awk -v n=$((n-1)) '$2==n {print $NF+1}')
[ -n "$first" ] || first=1
echo "readback $n: draws $first..$last"
rm -rf /t6/img/dz /t6/img/ds; mkdir -p /t6/img/dz /t6/img/ds
LIBGL_ALWAYS_SOFTWARE=1 VFRONT_DRAWDUMP=/t6/img/dz:$first:$last $R $cap > /dev/null 2>&1
VFRONT_DRIVER=softpipe VFRONT_DRAWDUMP=/t6/img/ds:$first:$last $R $cap > /dev/null 2>&1
d=$(python3 /t6/vfront/tools/firstdiff.py /t6/img/dz /t6/img/ds ${THRESHOLD:-0.3} | tail -1)
echo "$d"
dn=$(echo "$d" | sed -n 's/^draw-0*\([0-9]*\)\.ppm.*/\1/p')
[ -n "$dn" ] || exit 0
python3 /t6/vfront/tools/ppm2png.py /t6/img/draw-$dn.png /t6/img/dz/draw-$(printf %06d $dn).ppm /t6/img/ds/draw-$(printf %06d $dn).ppm
LIBGL_ALWAYS_SOFTWARE=1 VFRONT_DEBUG=cmd,shader $R $cap > /t6/log/drawdiff.txt 2>&1
awk -v d=$dn '/DRAW_VBO/ {k++} k==d-1 {print}' /t6/log/drawdiff.txt | grep -v "^shader:\|^ \|^$" | grep -v "TGSI" | head -40
echo "---- shaders bound"
for h in $(awk -v d=$dn '/DRAW_VBO/ {k++} k<d && /bind .* shader/ {s[$3]=$5} END {for (x in s) print s[x]}' /t6/log/drawdiff.txt); do
	awk -v h="$h" '/^vfront: shader object/ {p = ($4 == h":")} p && !/^shader:/ {print} /^shader:/ {p=0}' /t6/log/drawdiff.txt | head -60
done
