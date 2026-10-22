#!/bin/bash
# In the chroot: run CLIENT through vfront-vtest with a capture (under Xvfb when X=1), replay the capture into
# vfront and into vrend (the reference), compare the images every readback produced.
#   cap-compare.sh NAME 'CLIENT ARGS' [TIMEOUT_S]
# Results: /t6/img/NAME/{vfront,vrend,png}, /t6/log/NAME-*.txt
name=$1; client=$2; to=${3:-20}
mkdir -p /t6/img/$name /t6/log
if [ "$X" = 1 ]; then
	Xvfb :7 -screen 0 1024x768x24 -nolisten tcp > /t6/log/xvfb.log 2>&1 &
	xpid=$!
	sleep 1
	export CENV="DISPLAY=:7 $CENV"
fi
bash /t6/vfront/scripts/vt-run.sh "$client" $name $to > /t6/log/$name-run.txt 2>&1
[ -n "$xpid" ] && kill $xpid
tail -8 /t6/log/$name-run.txt
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
rm -rf /t6/img/$name/vfront /t6/img/$name/vrend /t6/img/$name/png
LIBGL_ALWAYS_SOFTWARE=1 /t6/build-native/src/gallium/frontends/vfront/vfront-replay /t6/cap/$name.vfcap --dump /t6/img/$name/vfront --every ${EVERY:-1} --times 2>&1 | tail -4
EGL_PLATFORM=surfaceless /t6/vrend-replay /t6/cap/$name.vfcap --dump /t6/img/$name/vrend --every ${EVERY:-1} 2>&1 | tail -2
python3 /t6/vfront/tools/imgcmp.py /t6/img/$name/vfront /t6/img/$name/vrend --png /t6/img/$name/png > /t6/log/$name-cmp.txt 2>&1
tail -${CMPLINES:-6} /t6/log/$name-cmp.txt
