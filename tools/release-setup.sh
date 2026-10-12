#!/bin/bash
# The one command for the release, run on the Mac: copy this tree to the build
# VM (~/wt/release), build the release there and start it with Docker (docker/release.sh), on
# http://localhost:8080/ (Lima forwards the VM's ports).
#   tools/release-setup.sh [STEP...]       default: all (docker/release.sh's steps: bundle root disk code dist up)
#   tools/release-setup.sh down            stop the containers
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
limactl shell --workdir / rvwasm bash -c "cd ~/wt/release && RV_RELEASE_ID='$ID' NQ_INFO_COMMIT='$NQ' docker/release.sh $*"
