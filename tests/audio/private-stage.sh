#!/bin/bash
# A stage of a tree that is not served: tools/stage.sh's boot image, built in the build VM from TREE (as the VM has
# it, ~/wt/NAME) into ~/rvwasm/stage-NAME and ~/rvwasm/initramfs-NAME.cpio.gz, without the part that touches
# what the dev server serves (~/rvwasm/www, the workspace runtime). For node-machine runs of a merged tree
# (tests/audio/run.sh with NM_INITRAMFS=...) while the owner tests another stage.
#   tests/audio/private-stage.sh ~/wt/NAME          (in the build VM)
set -e
S=$(realpath "$1")
NAME=$(basename "$S")
W=~/rvwasm
T=$(mktemp)
# stage.sh up to the dev page's part, with its stage directory renamed.
sed -n '1,/^# The dev page and our changes to the JS runtime\./p' "$S/tools/stage.sh" | sed '$d' |
	sed -e "s|^S=.*|S=$S|" -e "s|\$W/stage\b|\$W/stage-$NAME|g" -e "s|\"\$W/stage\"|\"\$W/stage-$NAME\"|g" > $T
echo "rm -f \"\$W/initramfs-$NAME.cpio.gz\"; \"\$S/tools/mk-initramfs.sh\" \"\$W/stage-$NAME\" \"\$W/initramfs-$NAME.cpio.gz\"" >> $T
grep -q "\$W/www" $T && { echo "private-stage: the cut of stage.sh still writes to www"; grep -n "\$W/www" $T; exit 1; }
. $W/env.sh
bash $T
rm -f $T
ls -la $W/initramfs-$NAME.cpio.gz
