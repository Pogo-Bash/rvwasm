#!/bin/bash
# Set the dev server up for the manual test sheets, the owner's sheet for the Vulkan / WebGPU program (VK-20). Run on the Mac.
#
#   gpu/tools/vulkan-setup.sh stage      back up the shared stage in the build VM, stage THIS tree on the dev server (:8000), put the
#                                                stamped rvgpu executor on it, start the helper servers, check it
#   gpu/tools/vulkan-setup.sh restore    put the backed-up stage back, exactly, and check it is the same; stop everything started
#   gpu/tools/vulkan-setup.sh check      what the owner's browser will get, from the dev server on localhost:8000 and, under Node
#                                                (no browser), the staged machine on the real executor: vulkaninfo and vkcube (--no-node: files only)
#   gpu/tools/vulkan-setup.sh status     staged or not, what is listening
#   gpu/tools/vulkan-setup.sh build      build the rvgpu executor from this tree in the VM (gpu/webgpu/build/vm.sh wasm.sh, -j6; HEAVY)
#   gpu/tools/vulkan-setup.sh serve MODE the site server on :8002 whose rv-site.json says gpu_exec MODE (vrend, webgpu or auto): VK-13's steps
#   gpu/tools/vulkan-setup.sh vk16 on|off  the derived disk vk16 (Blender installed, VK-16) as ?disk=vk16 on :8000; it is also served on :8016
#   gpu/tools/vulkan-setup.sh native     the two native-Vulkan test pages (VK-19b, VK-21): :8123 (vkcube) and :8124 (zink gears)
#
# OPTIONS (stage, restore)   --wait  wait (up to 40 min) for the VM and the Mac to be free of other test machines instead of stopping
#                            --force restore over a stage that was changed after this one (discards that change)
#                            --no-node  stage: skip the Node run of the staged machine (files are still checked)
# ENVIRONMENT                RVGPU_BUILD  the rvgpu build in the VM to serve (~/rvwasm/webgpu/b/NAME; default: vk-20)
#
# WHAT STAGING DOES TO THE SHARED STAGE. The dev server on :8000 (the VM's ~/rvwasm/www) hands every browser and every `tools/gpu-nm.sh`
# the same kernel, runtime and initramfs, and tools/stage.sh replaces them from whatever tree runs it. So before it replaces them this
# script
#   1. refuses while anything uses them: a tools/node-machine.js (or gpu-nm / gpu-matrix) that talks to localhost:8000, a test browser
#      (with-browser lock, headless Chrome for Testing), a held node-machine slot, a stage.sh in the VM;
#   2. holds both machine-wide node-machine slots (/tmp/rvwasm-nm-slot.N, tools/nm-slot.sh's protocol) while it swaps, so none starts in between;
#   3. copies what stage.sh replaces to ~/rvwasm/stage-backup/vulkan (stage/, www/ except disk/, linux-wasm/runtime, guest-extra) with a
#      manifest of every file (mode, time, size, sha256) and every link;
#   4. runs tools/stage.sh of this tree (the VM copy ~/wt/vulkan-stage; its rv, venus files, matrix script, gpuserver, window server and
#      page are what the initramfs and www/ get), then installs the rvgpu executor (stamped and clean, or it stops) as www/rvgpu/ and
#      www/rvgpu-worker.js, and seals the result in a second manifest;
#   5. if stage.sh or the executor fails it puts the backup back by itself.
# `restore` refuses when the stage no longer matches what was sealed (somebody staged after this one) unless --force; it puts back
# everything, deletes the backup only after the manifest of the live stage equals the backup's, and stops the helper servers.
# The build caches (build-*, stage-vulkan.log) are not part of it. Never run tools/stage.sh on the shared VM stage by hand around this.
set -e
WT=$(cd "$(dirname "$0")/../.." && pwd)
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
SSHCFG=$HOME/.lima/rvwasm/ssh.config
TREE=vulkan-stage                                   # this tree's name in the VM: ~/wt/vulkan-stage
RVBUILD=${RVGPU_BUILD:-vk-20}                      # the VM build made for the staged tree (gpu/webgpu/build/vm.sh from the vk-20 worktree)
VMSCRIPT=$WT/gpu/tools/vulkan-stage-vm.sh
URL=${GPU_NM_URL:-http://localhost:8000}
NMSLOT=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/nm-slot.sh

WAIT= ; FORCE= ; NONODE=
cmd=${1:-help}; [ $# -eq 0 ] || shift
args=()
for a in "$@"; do
	case $a in
		--wait) WAIT=1 ;;
		--force) FORCE=1 ;;
		--no-node) NONODE=1 ;;
		*) args+=("$a") ;;
	esac
done

# --- the VM: run the VM script with a subcommand (it comes on stdin, so the VM never runs an older copy)
vmrun() {
	limactl shell --workdir / rvwasm env TREE=$TREE RVGPU_BUILD=$RVBUILD FORCE=$FORCE STAGE_WHAT="$(git -C "$WT" rev-parse --short HEAD)_$(git -C "$WT" rev-parse --abbrev-ref HEAD)" bash -s "$@" < "$VMSCRIPT"
}
vmsh() { limactl shell --workdir / rvwasm bash -c "$1"; }
say() { echo "vulkan-setup: $*"; }
die() { echo "vulkan-setup: $1" >&2; exit "${2:-1}"; }

# --- who uses the shared stage right now (prints the users; returns 0 when there are some)
users() {
	local found=0 line
	# node-machines that talk to the dev server (the default is localhost:8000; --server elsewhere is not ours)
	while read -r line; do
		case $line in */bin/sh*nm-slot.sh*|*nm-slot.sh\ *) continue ;; esac
		case $line in *tools/node-machine.js*) ;; *) continue ;; esac
		case $line in *--server\ *) case $line in *localhost:8000*|*127.0.0.1:8000*) ;; *) continue ;; esac ;; esac
		echo "  node-machine on the dev server: ${line:0:170}"; found=1
	done < <(ps -axo pid=,args= | grep -v grep)
	ps -axo pid=,args= | grep -E 'tools/(gpu-nm|gpu-matrix)\.sh' | grep -v grep | grep -v nm-slot | sed 's/^/  gpu-nm run: /' | cut -c1-190 | grep . && found=1
	for s in 1 2; do
		d=/tmp/rvwasm-nm-slot.$s
		[ -d $d ] && kill -0 "$(cat $d/pid 2> /dev/null)" 2> /dev/null && [ "$(cat $d/pid)" != "$$" ] && { echo "  node-machine slot $s held by pid $(cat $d/pid)"; found=1; }
	done
	if [ -d /tmp/rv-browser.lock ] && kill -0 "$(cat /tmp/rv-browser.lock/pid 2> /dev/null)" 2> /dev/null; then echo "  test browser lock held by $(cat /tmp/rv-browser.lock/who 2> /dev/null)"; found=1; fi
	pgrep -f 'MacOS/Google Chrome for Testing --headless' > /dev/null && { echo "  a headless Chrome for Testing is running"; found=1; }
	local p
	p=$(vmsh "pgrep -af 'tools/stage\\.sh|mk-initramfs|patch-runtime\\.py' | grep -v pgrep | grep -v bash.-s" 2> /dev/null || true)
	[ -z "$p" ] || { echo "  in the VM: $p"; found=1; }
	[ $found = 1 ]
}

held=
# Both node-machine slots at once, or none: never hold one while waiting for the other (another run would queue behind a waiter that
# does nothing; with two waiters that is a deadlock). The slots are held only around the swap itself, and released before the Node run of
# `check` (gpu-nm.sh takes its own through nm-slot.sh).
try_slots() {
	local s d
	for s in 1 2; do
		d=/tmp/rvwasm-nm-slot.$s
		if [ -d $d ] && ! kill -0 "$(cat $d/pid 2> /dev/null)" 2> /dev/null; then rm -rf $d; fi
		if mkdir $d 2> /dev/null; then echo $$ > $d/pid; held="$held $s"; fi
	done
	[ "$(echo $held | wc -w | tr -d ' ')" = 2 ] && return 0
	release_slots
	return 1
}
release_slots() { local s; for s in $held; do rm -rf /tmp/rvwasm-nm-slot.$s; done; held=; }
trap release_slots EXIT

# Wait (polling, holding nothing) for a moment when nothing uses the stage and both slots are free, and take them in that moment.
# Without --wait: one try. With --wait: up to 40 minutes, every 10 s.
claim() {
	local limit=$([ -n "$WAIT" ] && echo 2400 || echo 0) waited=0 out
	while :; do
		if ! out=$(users); then
			if try_slots; then
				# (a run may have started between the two looks: look again with the slots held; they keep new ones out)
				out=$(users) || return 0
				release_slots
			else
				out="  (both node-machine slots are not free)"
			fi
		fi
		[ $waited -lt $limit ] || { echo "$out" >&2; die "the shared stage is in use (above): not touching it. Run again later, or with --wait." 2; }
		[ $((waited % 300)) -ne 0 ] || say "waiting (holding nothing): $(echo "$out" | head -3 | tr '\n' ';')"
		sleep 10; waited=$((waited + 10))
	done
}

# --- the executor: the VM's build, the stamp rule, and the stamp's commit against this tree
executor_info() {
	local tmp
	tmp=$(mktemp -d)
	scp -q -F "$SSHCFG" lima-rvwasm:rvwasm/webgpu/b/$RVBUILD/wasm/src/webgpu/rvgpu/rvgpu.wasm "$tmp/rvgpu.wasm" 2> /dev/null || { rm -rf "$tmp"; die "no rvgpu build '$RVBUILD' in the VM (gpu/tools/vulkan-setup.sh build, or RVGPU_BUILD=NAME)"; }
	"$WT/tools/rvgpu-gate.sh" "$tmp/rvgpu.wasm" || { rm -rf "$tmp"; die "rvgpu build '$RVBUILD' is not stamped and clean: the executor is refused"; }
	local commit
	commit=$(node "$WT/tools/rvgpu-stamp.js" "$tmp/rvgpu.wasm" | awk '$1 == "commit" {print $2}')
	rm -rf "$tmp"
	git -C "$WT" cat-file -e "$commit^{commit}" 2> /dev/null || die "the executor's stamp names commit $commit, which this repository does not have"
	if ! git -C "$WT" diff --quiet "$commit" HEAD -- gpu/webgpu patches; then
		die "this tree's gpu/webgpu or patches differ from commit $commit that the executor was built from: gpu/tools/vulkan-setup.sh build"
	fi
	if [ -n "$(git -C "$WT" status --porcelain -- gpu/webgpu patches)" ]; then die "uncommitted changes under gpu/webgpu or patches: commit them and build"; fi
	say "executor build $RVBUILD: stamped, clean, commit ${commit:0:12} (gpu/webgpu and patches are the same in this tree)"
}

sync_tree() {
	vmsh "mkdir -p ~/wt/$TREE/third_party; [ -e ~/wt/$TREE/third_party/berkeley-softfloat-3 ] || ln -s ${RVWASM_MAIN:-$HOME/dev/rvwasm}/third_party/berkeley-softfloat-3 ~/wt/$TREE/third_party/berkeley-softfloat-3"
	( cd "$WT" && rsync -a --delete --exclude .git --exclude 'tools/chrome' --exclude 'tools/lima' --exclude 'tools/dl' --exclude node_modules --exclude third_party \
		--exclude '/build' --exclude '/linux-wasm' -e "ssh -F $SSHCFG" ./ lima-rvwasm:wt/$TREE/ )
	say "tree $(git -C "$WT" rev-parse --short HEAD) ($(git -C "$WT" rev-parse --abbrev-ref HEAD)) copied to ~/wt/$TREE in the VM$([ -n "$(git -C "$WT" status --porcelain | grep -v '^??')" ] && echo ', with uncommitted changes')"
}

# --- check: files from the dev server, and the staged machine under Node
check_files() {
	local bad=0 f n
	for f in dev.html gpu-exec.js rvgpu-worker.js rvgpu/rvgpu.js rvgpu/rvgpu.wasm initramfs-wasm64_nommu.cpio.gz vdec.js; do
		n=$(curl -s -m 60 -o /dev/null -w '%{size_download}' "$URL/$f")
		[ "$n" -gt 1000 ] && say "  ok  $f ($n bytes)" || { say "  BAD $f ($n bytes from $URL)"; bad=1; }
	done
	# the page is this tree's: dev.html, gpu-exec.js and rvgpu-worker.js equal the tree's own
	for f in dev.html gpu-exec.js rvgpu-worker.js; do
		curl -s -m 60 "$URL/$f" | cmp -s - "$WT/tools/www/$f" && say "  ok  $f is this tree's" || { say "  BAD $f differs from this tree's tools/www/$f"; bad=1; }
	done
	# the executor served is stamped and clean
	local tmp; tmp=$(mktemp -d)
	curl -s -m 120 -o "$tmp/rvgpu.wasm" "$URL/rvgpu/rvgpu.wasm"
	"$WT/tools/rvgpu-gate.sh" "$tmp/rvgpu.wasm" && say "  ok  rvgpu.wasm served is stamped and clean: $(node "$WT/tools/rvgpu-stamp.js" "$tmp/rvgpu.wasm" | awk '$1=="branch"||$1=="commit"{printf "%s ", $2}')" || { say "  BAD the served rvgpu.wasm is not stamped and clean"; bad=1; }
	# the initramfs has this tree's rv (VK-14: PRIME import across opens) and the Vulkan files (VK-08a), the matrix script (VK-17), QT_VULKAN_LIB (VK-27)
	curl -s -m 120 -o "$tmp/i.cpio.gz" "$URL/initramfs-wasm64_nommu.cpio.gz"
	( cd "$tmp" && mkdir x && cd x && gzip -dc ../i.cpio.gz | cpio -id --quiet 2> /dev/null ) || true
	if [ -f "$tmp/x/bin/rv" ]; then
		if strings -a "$tmp/x/bin/rv" | grep -q 'PRIME import.*not done yet'; then say "  BAD the initramfs' rv is old: no PRIME import across opens (mpv / Chromium on the render node fail)"; bad=1
		else say "  ok  the initramfs' rv has PRIME import ($(shasum -a 256 "$tmp/x/bin/rv" | cut -c1-12))"; fi
	else say "  BAD no bin/rv in the initramfs"; bad=1; fi
	for f in usr/lib/rvgpu/venus/libvulkan_virtio.so usr/lib/rvgpu/venus/virtio_icd.json usr/lib/rvgpu/venus/vkcube usr/lib/rvgpu/venus/vulkaninfo usr/lib/rvgpu/matrix/gpu-matrix-guest.sh usr/lib/rvgpu/bin/stageb-trace usr/lib/wlserver/desktop usr/lib/rvgpu/server.js; do
		[ -e "$tmp/x/$f" ] && say "  ok  initramfs has /$f" || { say "  BAD initramfs lacks /$f"; bad=1; }
	done
	cmp -s "$tmp/x/usr/lib/wlserver/desktop" "$WT/tools/x/desktop" && say "  ok  /usr/lib/wlserver/desktop is this tree's (QT_VULKAN_LIB: $(grep -c QT_VULKAN_LIB "$tmp/x/usr/lib/wlserver/desktop") lines)" || { say "  BAD desktop differs from this tree's"; bad=1; }
	cmp -s "$tmp/x/usr/lib/rvgpu/server.js" "$WT/gpu/server/server.js" && say "  ok  gpuserver is this tree's (VK-25's blob fix, VK-15's lazy start)" || { say "  BAD gpuserver differs from this tree's"; bad=1; }
	rm -rf "$tmp"
	# the helper servers of the sheet
	vmsh "for u in http://10.0.5.1:8019/v.tgz http://10.0.5.1:8020/m.tgz; do printf '%s %s bytes\n' \$u \$(curl -s -m 5 \$u | wc -c); done"
	return $bad
}

check_node() {
	local n=$WT/build/stage-check rc=0
	rm -rf "$n/rvgpu-page"; mkdir -p "$n/rvgpu-page/rvgpu"
	for f in rvgpu/rvgpu.js rvgpu/rvgpu.wasm rvgpu-worker.js; do curl -sf -m 120 -o "$n/rvgpu-page/$f" "$URL/$f"; done
	say "Node: the staged machine (initramfs, kernel, runtime fetched now from $URL, nothing of this tree on top) with the served executor"
	for prog in vulkaninfo vkcube; do
		( cd "$WT" && GPU_NM_FRESH=1 GPU_NM_DIR=$n RVGPU_DIR=$n/rvgpu-page tools/gpu-nm.sh --staged $([ "$prog" = vulkaninfo ] && echo -t 120) "$prog" 2>&1 | grep -E 'PASS|FAIL|Venus|presents|WARN|Selected|MATRIX_RESULT' | head -8 ) || rc=1
	done
	return $rc
}

case $cmd in
	stage)
		executor_info
		[ -z "$(git -C "$WT" status --porcelain gpu tools docker wlserver rv hostnode 2> /dev/null | grep -v '^??')" ] || say "WARNING: uncommitted changes in this tree are staged too"
		claim
		sync_tree
		say "staging (stage.sh takes a few minutes; the log is ~/rvwasm/stage-vulkan.log in the VM)"
		vmrun stage || { release_slots; die "staging failed; the shared stage was put back (see above)"; }
		release_slots
		vmrun helpers
		say "checking what is served"
		check_files || die "the staged files are not as expected (above); 'restore' puts the shared stage back" 1
		if [ -z "$NONODE" ]; then check_node || die "the Node run of the staged machine failed (above)"; fi
		cat <<EOF

READY. The dev server on :8000 serves the vulkan tree. Open in Chrome:
  http://localhost:8000/dev.html?disk=daily&exec=webgpu
and follow the manual test sheets When you are done:
  gpu/tools/vulkan-setup.sh restore
EOF
		;;
	restore)
		# (nothing to claim for the put-back itself except that no machine is mid-fetch: the same rule)
		claim
		vmrun restore || { release_slots; die "restore failed (above): the backup is still in ~/rvwasm/stage-backup/vulkan"; }
		release_slots
		[ ! -f "$WT/build/native/pids" ] || "$0" native-stop || true
		say "restored. The shared stage is what it was."
		;;
	check)
		executor_info
		check_files
		[ -n "$NONODE" ] || check_node
		;;
	status) vmrun status ;;
	build)
		( cd "$WT" && git diff --quiet HEAD -- gpu/webgpu patches ) || die "commit your changes under gpu/webgpu and patches first (the stamp says dirty otherwise)"
		cd "$WT" && GPU_BUILD=$RVBUILD JOBS=6 gpu/webgpu/build/vm.sh gpu/webgpu/build/wasm.sh
		;;
	serve) vmrun serve "${args[@]}" ;;
	vk16) vmrun vk16 "${args[@]}" ;;
	native)
		# VK-19b / VK-21: the native-Vulkan pages come from the nqgl tree's kit; the machines were built in those cards' worktrees
		VKT=${NQ_VK_TREE:-$HOME/dev/rvwasm-wt/vk-19b}; ZKT=${NQ_ZINK_TREE:-$HOME/dev/rvwasm-wt/vk-21}
		mkdir -p "$WT/build/native"; : > "$WT/build/native/pids"
		( cd "$VKT" && VKNATIVE=1 tests/wasmdyn/browser-prepare.sh > /dev/null && rm -rf "$WT/build/native/vk" && cp -R build/wasmdyn/browser "$WT/build/native/vk" )
		( cd "$ZKT" && ZINK=1 tests/wasmdyn/browser-prepare.sh > /dev/null && rm -rf "$WT/build/native/zink" && cp -R build/wasmdyn/browser "$WT/build/native/zink" )
		nohup python3 -u "$VKT/tests/wasmdyn/browser-serve.py" "$WT/build/native/vk" 8123 > "$WT/build/native/vk.log" 2>&1 & echo $! >> "$WT/build/native/pids"
		nohup python3 -u "$ZKT/tests/wasmdyn/browser-serve.py" "$WT/build/native/zink" 8124 > "$WT/build/native/zink.log" 2>&1 & echo $! >> "$WT/build/native/pids"
		sleep 2; head -1 "$WT/build/native/vk.log" "$WT/build/native/zink.log"
		;;
	native-stop)
		[ -f "$WT/build/native/pids" ] && { kill $(cat "$WT/build/native/pids") 2> /dev/null || true; rm -f "$WT/build/native/pids"; say "native pages stopped"; }
		;;
	*) sed -n '2,/^set -e/p' "$0" | sed '$d'; [ "$cmd" = help ] || exit 2 ;;
esac
