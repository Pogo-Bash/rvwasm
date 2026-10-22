#!/bin/bash
# A LOCAL dist and site for the browser gates (nothing is published, nothing leaves this Mac): the page files, runtime patches and boot
# image of THIS tree on the live dist's kernel/emulator/disk manifests, with the nqs disk built in the VM (nativeqt/shared/build-nqs-image.sh,
# NQS_OUT/NQS_PIECES) in place of the live one.
#   tests/wasmdyn/make-local-dist.sh [NQS_NAME]      NQS_NAME: the VM's ~/rvwasm/www/disk/NAME and ~/rvwasm/NAME/dl (default nqs-ndcb)
# Env: BASE_TREE (VM tree whose dist is the starting point; default wt/jmship = the live site of 2026-10-14), VMTREE (default wt/ndcb2:
# this tree's copy in the VM), OUT (default ~/dev/rvwasm-wt/dist-ndcb), RV_BIN unset (the live /bin/rv stays).
set -e
WT=$(cd "$(dirname "$0")/../.." && pwd)
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
NQS=${1:-nqs-ndcb}
BASE=${BASE_TREE:-wt/jmship}
VMTREE=${VMTREE:-wt/ndcb2}
OUT=${OUT:-$HOME/dev/rvwasm-wt/dist-ndcb}
SSHC="ssh -F $HOME/.lima/rvwasm/ssh.config"
VM=lima-rvwasm
vm() { $SSHC $VM "$@"; }
echo "=== this tree into the VM ($VMTREE)"
vm "mkdir -p $VMTREE"
(cd "$WT" && git ls-files -z | rsync -a --from0 --files-from=- -e "$SSHC" ./ $VM:$VMTREE/)
echo "=== the dist in the VM (wt/ndcb-dist): $BASE's, with the disk $NQS"
vm "set -e
rm -rf ~/wt/ndcb-dist; mkdir -p ~/wt/ndcb-dist/disk
cp -a ~/$BASE/dist/www ~/$BASE/dist/RELEASE.txt ~/$BASE/dist/server.py ~/wt/ndcb-dist/
cp -a ~/$BASE/dist/disk/release ~/$BASE/dist/disk/home ~/wt/ndcb-dist/disk/
rm -rf ~/wt/ndcb-dist/www/dl; cp -a ~/rvwasm/$NQS/dl ~/wt/ndcb-dist/www/dl
cp -al ~/rvwasm/www/disk/$NQS ~/wt/ndcb-dist/disk/nqs
cd ~/$VMTREE && docker/patch-dist.sh ~/wt/ndcb-dist
id=\$(sed -n 's/.*\"id\": *\"\\([0-9a-f]*\\)\".*/\\1/p' ~/wt/ndcb-dist/disk/nqs/manifest.json | head -1)
grep -v -e '^disk nqs:' -e '^dl:' ~/wt/ndcb-dist/RELEASE.txt > ~/wt/ndcb-dist/RELEASE.txt.new || true
echo \"disk nqs: id \$id, shared libraries for the native apps (mounted at /opt/nq-shared)\" >> ~/wt/ndcb-dist/RELEASE.txt.new
echo \"dl: \$(grep -c . ~/rvwasm/$NQS/dl.log) libraries, local build $NQS\" >> ~/wt/ndcb-dist/RELEASE.txt.new
mv ~/wt/ndcb-dist/RELEASE.txt.new ~/wt/ndcb-dist/RELEASE.txt"
echo "=== to the Mac ($OUT)"
mkdir -p "$OUT"
rsync -a --delete -e "$SSHC" --exclude 'disk/release/[0-9]*' --exclude 'disk/home/[0-9]*' $VM:wt/ndcb-dist/ "$OUT/"
echo "=== done: serve it with RATE=bytes/s SITE_LOG=file python3 tests/wasmdyn/serve-local-site.py PORT $OUT"
cat "$OUT/RELEASE.txt"
