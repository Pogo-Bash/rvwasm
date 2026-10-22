#!/bin/bash
# Put the build VM's dist on the public server (docs/deploy.md "Updating the dist"), from the Mac (the VM has no key
# for the server).
#
#   [RV_BIN=path in the VM of an emulator built from this tree] [RV_FILES="image/path:tree/path ..." more boot-image files of this tree] tools/deploy.sh [-n] [SERVER]      SERVER: an ssh name (or $RV_SERVER); -n: show what would be copied, copy nothing
#
# Nothing is deleted on the server and nothing is built there; only files whose content differs are copied. In this
# order, so that the site is never half switched:
#   0. this tree's page files and /init into the build VM's dist (docker/patch-dist.sh, run in the VM): dev.html,
#      disk-worker.js, display.js, the runtime patches the dist lacks (tools/patch-runtime.py) and the boot image's /init are this tree's, whatever tree the VM built the dist from,
#      and the disks' manifests get their boot list (docker/boot, tools/r2-publish.py --boot-only: the `id` stays);
#   1. the dist from the VM (~/wt/release/dist) into ./dist on the Mac, everything but the disks' pieces;
#   2. for each disk whose `id` is not the one the server has: its pieces, VM -> Mac -> server. (The same id is the
#      same disk: its pieces are not copied again. The server keeps its own pieces even when the bucket
#      (tools/r2-publish.py) has them too: the page falls back to them.)
#   3. the page, kernel and the rest (dist/www, RELEASE.txt), compared by content (--checksum);
#   4. the manifests, last, each replaced in one step: a manifest names the disk (`id`) and, from the second
#      release on, where its pieces are (`base`) and what they are called (`hashes`). Browsers read it fresh at
#      every boot, so this is the moment the new disk or the new place for the pieces takes effect.
# So the server has exactly what this prints, and nothing needs patching on it by hand. With -n steps 0 and 1 still
# run (they only touch the build VM's dist and ./dist here, nothing that is live) and the steps that write to the
# server list what they would copy (rsync -i, so each file and why).
# The GPU executor kill switch is not part of this: it is RV_GPU_EXEC in the server's .env, `docker compose up -d --no-deps caddy`
# (docs/deploy.md "GPU executor switch"); set it to vrend to turn the WebGPU executor off for every visitor, no deploy needed.
# tools/release-setup.sh makes the dist, with the pieces on the bucket and the manifests saying so (publish).
set -e
WT=$(cd "$(dirname "$0")/.." && pwd)
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
DRY=
[ "$1" != -n ] || { DRY=-n; shift; }
SERVER=${1:-${RV_SERVER:?name the server (an ssh name) as the first argument or in RV_SERVER}}
DEST=${RV_DEST:-rvwasm/dist}
LOCAL=${RV_DIST_LOCAL:-$WT/dist}
# RV_VM_TREE: the tree in the build VM whose dist is copied (default wt/release); a private one keeps a deploy from changing the dist that other
# releases are made from (see docs/deploy.md).
VMTREE=${RV_VM_TREE:-wt/release}
VM=lima-rvwasm:$VMTREE/dist
VMSSH="ssh -F $HOME/.lima/rvwasm/ssh.config"
PIECES='[0-9][0-9][0-9][0-9][0-9][0-9]'
say() { echo "=== deploy: $*"; }
id_of() { sed -n 's/.*"id": *"\([0-9a-f]*\)".*/\1/p' "$1" | head -1; }

limactl list rvwasm 2> /dev/null | grep -q Running || { echo "deploy: the build VM is not running (limactl start rvwasm)" >&2; exit 1; }
mkdir -p "$LOCAL"

say "0. this tree's page files and /init into the build VM's dist"
(cd "$WT" && rsync -aR -e "$VMSSH" docker/patch-dist.sh tools/r2-publish.py docker/boot tools/initramfs-init tools/x/desktop tools/x/nqs-copy tools/patch-runtime.py tools/www/dev.html tools/www/probe.html tools/www/disk-worker.js tools/www/display.js tools/www/gpu-exec.js tools/www/vdec.js tools/www/vdec-worker.js tools/www/rvgpu-worker.js tools/rvgpu-stamp-check.py gpu/tools/stageb-trace.py gpu/tools/gpu-matrix-guest.sh gpu/tools/matrix tools/www/jitcache.js tools/www/hostopen.js tools/www/hostopen.css tools/x/xdg hostopen/bin hostopen/boot.js hostopen/rv-host-open.desktop tools/debian-enter gpu/server wlserver/plasmashell.js wlserver/xwm.js wlserver/layershell.js wlserver/compositor.js lima-rvwasm:$VMTREE/)
$VMSSH lima-rvwasm "cd $VMTREE && RV_BIN='${RV_BIN:-}' RV_ONLY='${RV_ONLY:-}' RV_FILES='${RV_FILES:-}' RV_RVGPU_DIR='${RV_RVGPU_DIR:-}' RV_VENUS_DIR='${RV_VENUS_DIR:-}' RV_GPU_BINS='${RV_GPU_BINS:-}' docker/patch-dist.sh dist"

say "1. the dist from the VM (not the pieces)"
rsync -a --partial --exclude "disk/*/$PIECES" -e "$VMSSH" "$VM/" "$LOCAL/"

for d in "$LOCAL"/disk/*/; do
	name=$(basename "$d")
	have=$(ssh "$SERVER" "cat $DEST/disk/$name/manifest.json 2> /dev/null" | sed -n 's/.*"id": *"\([0-9a-f]*\)".*/\1/p' | head -1)
	if [ "$have" = "$(id_of "$d/manifest.json")" ]; then
		say "2. disk $name: the server has this disk ($have), pieces not copied"
	else
		say "2. disk $name: new on the server (it has '${have:-nothing}'): the pieces"
		rsync -aW --partial $DRY --exclude manifest.json -e "$VMSSH" "$VM/disk/$name/" "$LOCAL/disk/$name/"   # (whole files: the delta check on 1 MiB pieces crawled)
		rsync -a -c --partial $DRY --exclude manifest.json "$LOCAL/disk/$name/" "$SERVER:$DEST/disk/$name/"   # (-c: only changed pieces travel)
	fi
done

say "3. the page and the rest"
rsync -a --partial --checksum ${DRY:+-in} --exclude /disk/ "$LOCAL/" "$SERVER:$DEST/"

say "4. the manifests, last"
for d in "$LOCAL"/disk/*/; do
	name=$(basename "$d")
	rsync -a --checksum ${DRY:+-in} "$d/manifest.json" "$SERVER:$DEST/disk/$name/manifest.json"
done
say "done"
