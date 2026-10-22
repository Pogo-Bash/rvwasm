#!/bin/bash
# In the chroot: run glmark2-es2 with the given scenes (0.3 s each) through vfront, compare with vrend; print
# the number of differing images.   glmark-pair.sh NAME SCENE...
name=$1; shift
args="-s 400x300"
for b in "$@"; do args="$args -b $b:duration=${D:-0.3}"; done
X=1 EVERY=${EVERY:-1} CMPLINES=1 bash /t6/vfront/scripts/cap-compare.sh $name "glmark2-es2 $args" 120 > /t6/log/$name-pair.txt 2>&1
echo "$name: $(grep -c DIFFERENT /t6/log/$name-cmp.txt) different of $(grep -c ppm /t6/log/$name-cmp.txt); first: $(grep -m1 DIFFERENT /t6/log/$name-cmp.txt | cut -c1-14)"
rm -f /t6/cap/$name.vfcap
