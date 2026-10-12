#!/bin/bash
# The one command for the release, run on the Mac: copy this tree to the build
# VM (~/wt/release), build the release there and start it with Docker (docker/release.sh), on
# http://localhost:8080/ (Lima forwards the VM's ports).
#   tools/release-setup.sh [STEP...]       default: all (docker/release.sh's steps: bundle root disk code dist publish up)
#   tools/release-setup.sh down            stop the containers
#   tools/release-setup.sh publish         only the pieces to object storage (below)
# The disk's pieces also go to object storage (Cloudflare R2, docs/deploy.md "Pieces from object storage") when
# ${RV_R2_ENV:-~/.config/rvwasm/r2.env} exists (R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY): the
# steps that make the disk (dist) are followed by `publish`, which hashes the pieces, uploads the ones the bucket
# lacks and writes the base and hashes into the dist's manifests, last. The credentials are handed to that one command
# on its standard input: they are not in an argument, a file in the VM or its image, or the build's environment. Without
# the file nothing is uploaded and the dist serves its pieces itself. RV_PIECE_BASE: where the page fetches them from.
# Takes 15-30 minutes the first time (the kernel and the disk), a few after that.
set -e
WT=$(cd "$(dirname "$0")/.." && pwd)
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
limactl list rvwasm 2> /dev/null | grep -q Running || { echo "release-setup: the build VM is not running (limactl start rvwasm)" >&2; exit 1; }
ID="$(git -C "$WT" rev-parse --abbrev-ref HEAD) $(git -C "$WT" log -1 --format='%h %cd' --date=format:'%F %R')"
[ -z "$(git -C "$WT" status --porcelain --untracked-files=no)" ] || ID="$ID (with uncommitted changes)"
# (the build VM only mounts the main checkout's directory tree: this tree goes to ~/wt/release; dist/ there is the
# release's, kept)
rsync -a --delete --exclude .git --exclude 'tools/chrome' --exclude 'tools/lima' --exclude 'tools/dl' --exclude node_modules \
	--exclude third_party --exclude build --exclude /dist --exclude /rvwasm-release.tar.gz \
	-e "ssh -F $HOME/.lima/rvwasm/ssh.config" "$WT/" lima-rvwasm:wt/release/
# (which nativeqt the native plasmashell is: the nativeqt tree's commit; the program's own date is added in the VM)
NQ="nativeqt $(git -C "${NQ_TREE:-$HOME/dev/rvwasm-wt/nativeqt}" log -1 --format='%h' 2> /dev/null || echo '(unknown commit)')"
R2ENV=${RV_R2_ENV:-$HOME/.config/rvwasm/r2.env}
PIECE_BASE=${RV_PIECE_BASE:-https://disk.plasmaontheweb.com/disk/}
publish=
if [ -r "$R2ENV" ]; then
	case " ${*:-all} " in *" all "* | *" dist "* | *" publish "*) publish=1 ;; esac
fi
if [ "$*" != publish ]; then
	limactl shell --workdir / rvwasm bash -c "cd ~/wt/release && RV_RELEASE_ID='$ID' NQ_INFO_COMMIT='$NQ' docker/release.sh $*"
fi
if [ -n "$publish" ]; then
	# (the build's own `publish` step, inside `all`, found no credentials and did nothing; this one has them)
	limactl shell --workdir / rvwasm bash -c "set -a; . /dev/stdin; set +a; cd ~/wt/release && RV_PIECE_BASE='$PIECE_BASE' docker/release.sh publish" < "$R2ENV"
elif [ "$*" = publish ]; then
	echo "release-setup: no credentials for object storage ($R2ENV)" >&2
	exit 1
fi
