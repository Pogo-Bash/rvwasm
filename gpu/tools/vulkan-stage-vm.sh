#!/bin/bash
# The build VM's half of gpu/tools/vulkan-setup.sh (VK-20). Not run by hand: the Mac script sends it to the VM
# on stdin (bash -s ARGS), so the VM never runs an older copy of it than the Mac's.
#
#   backup         copy what tools/stage.sh replaces to $BK (once; a second call keeps the first backup)
#   stage          backup, then tools/stage.sh of ~/wt/$TREE, then the rvgpu executor (build $RVGPU_BUILD), then seal
#   seal           record the staged state ($BK/staged-manifest.txt): restore refuses to run over a stage somebody else made after this
#   restore        put back exactly what backup took (FORCE=1: even over a stage somebody else made), check it, delete the backup
#   verify         the stage as it is against the backup's manifest (exit 0: identical)
#   status         is a backup there, what was staged, what is served
#   serve MODE     the site server on :8002 (docker/serve.py: rv-site.json with gpu_exec MODE = vrend|webgpu|auto) for VK-13's steps
#   helpers        the video bundle (:8019) and the matrix bundle (:8020) on the guest network's 10.0.5.1, for VK-14 and VK-17
#   vk16 on|off    disk vk16 (release + apt Blender, VK-16) under www/disk/vk16, so ?disk=vk16 works on :8000; its own server on :8016
#   stop           stop the servers this script started (serve, helpers), remove the vk16 link
#
# The shared stage is: $W/stage (the initramfs' content), $W/www except disk/ (the page, kernel, runtime, initramfs, rvgpu),
# $W/linux-wasm/runtime (stage.sh patches it: the runtime www/ links to) and $W/guest-extra (what stage.sh builds into it).
# Everything runs under flock, so two of these cannot interleave.
set -u
W=${RVWASM_WORK:-$HOME/rvwasm}
BK=$W/stage-backup/vulkan
TREE=${TREE:-vulkan-stage}                 # the Mac tree as the Mac script copied it: ~/wt/$TREE
RVGPU_BUILD=${RVGPU_BUILD:-}               # the rvgpu build to serve: ~/rvwasm/webgpu/b/NAME
LOG=$W/stage-vulkan.log

say() { echo "stage-vm: $*"; }
die() { echo "stage-vm: $1" >&2; exit "${2:-1}"; }

# What the stage consists of, as lines "d MODE PATH", "l TARGET PATH", "f MODE MTIME SIZE SHA256 PATH", sorted by path. Directory times
# are left out (they change with every file made in them; rsync -a puts them back anyway, the files' times are checked).
manifest() {
	(
		cd "$W" || exit 1
		local top="stage guest-extra linux-wasm/runtime www"
		find $top -path www/disk -prune -o -type d -printf 'd %m %p\n'
		find $top -path www/disk -prune -o -type l -printf 'l %l %p\n'
		find $top -path www/disk -prune -o -type f -printf 'f %m %Ts %s %p\n' | sort -k5 > /tmp/vk20-meta.$$
		find $top -path www/disk -prune -o -type f -print0 | xargs -0 sha256sum | sed 's/^\([0-9a-f]*\)  \(.*\)$/\1 \2/' | sort -k2 > /tmp/vk20-sha.$$
		# (join the two on the path, the last field of the first and the second of the other)
		awk 'NR==FNR { sha[$2] = $1; next } { p = substr($0, index($0, $5)); printf "%s %s %s %s %s %s\n", $1, $2, $3, $4, sha[p], p }' /tmp/vk20-sha.$$ /tmp/vk20-meta.$$
		rm -f /tmp/vk20-meta.$$ /tmp/vk20-sha.$$
	) | LC_ALL=C sort
}

lock() {
	exec 9> "$W/.vulkan-stage.lock"
	flock -w 900 9 || die "another vulkan-stage run holds $W/.vulkan-stage.lock"
}

busy_vm() {
	local p
	p=$(pgrep -af 'tools/stage\.sh|mk-initramfs|patch-runtime\.py' | grep -v pgrep | grep -v "vulkan-stage-vm" || true)
	[ -z "$p" ] || { echo "$p"; return 0; }
	return 1
}

backup() {
	if [ -e "$BK/manifest.txt" ]; then
		say "a backup is already there ($(head -1 "$BK/info.txt" 2> /dev/null)): kept as it is, restage on top of it"
		return 0
	fi
	if [ -e "$BK" ]; then die "$BK exists without a manifest: a backup that did not finish; look at it before anything is staged" 3; fi
	command -v rsync > /dev/null || die "no rsync in the VM"
	local avail
	avail=$(df -Pk "$W" | awk 'NR==2 {print $4}')
	[ "$avail" -gt 2000000 ] || die "less than 2 GB free for the backup"
	rm -rf "$BK.partial"
	mkdir -p "$BK.partial"
	rsync -a "$W/stage/" "$BK.partial/stage/"
	rsync -a --exclude /disk/ "$W/www/" "$BK.partial/www/"
	rsync -a "$W/linux-wasm/runtime/" "$BK.partial/runtime/"
	rsync -a "$W/guest-extra/" "$BK.partial/guest-extra/"
	manifest > "$BK.partial/manifest.txt"
	echo "backup of $(date '+%F %R'): $(wc -l < "$BK.partial/manifest.txt") entries, stage rv $(sha256sum "$W/stage/bin/rv" | cut -c1-12)" > "$BK.partial/info.txt"
	mv "$BK.partial" "$BK"
	say "backed up the shared stage to $BK ($(du -sh "$BK" | cut -f1), $(wc -l < "$BK/manifest.txt") entries)"
}

install_rvgpu() {
	[ -n "$RVGPU_BUILD" ] || die "RVGPU_BUILD is not set"
	local d=$W/webgpu/b/$RVGPU_BUILD/wasm/src/webgpu/rvgpu
	[ -e "$d/rvgpu.wasm" ] || die "no rvgpu build $RVGPU_BUILD ($d)"
	# (the same rule as tools/rvgpu-gate.sh, with Python: the VM has no Node)
	python3 "$HOME/wt/$TREE/tools/rvgpu-stamp-check.py" "$d/rvgpu.wasm" > /dev/null || die "rvgpu.wasm of build $RVGPU_BUILD is not stamped and clean (tools/rvgpu-stamp-check.py)"
	mkdir -p "$W/www/rvgpu"
	cp "$d/rvgpu.js" "$d/rvgpu.wasm" "$W/www/rvgpu/"
	cp "$HOME/wt/$TREE/tools/www/rvgpu-worker.js" "$W/www/rvgpu-worker.js"
	say "rvgpu executor: build $RVGPU_BUILD, $(python3 "$HOME/wt/$TREE/tools/rvgpu-stamp-check.py" --line "$d/rvgpu.wasm")"
}

seal() {
	[ -e "$BK/manifest.txt" ] || die "nothing to seal: no backup"
	manifest > "$BK/staged-manifest.txt"
	echo "staged $(date '+%F %R') from ${STAGE_WHAT:-?}" >> "$BK/info.txt"
}

do_stage() {
	lock
	if p=$(busy_vm); then die "somebody else is staging or patching the runtime in the VM:
$p" 2; fi
	backup || exit $?
	say "tools/stage.sh of ~/wt/$TREE (log $LOG)"
	if ! ( . "$W/env.sh"; "$HOME/wt/$TREE/tools/stage.sh" ) > "$LOG" 2>&1; then
		tail -15 "$LOG" >&2
		say "stage.sh failed: putting the shared stage back"
		do_restore_locked 1 || true
		exit 1
	fi
	if ! install_rvgpu; then
		say "no executor: putting the shared stage back"
		do_restore_locked 1 || true
		exit 1
	fi
	seal
	say "staged: initramfs $(stat -c %s "$W/www/initramfs-wasm64_nommu.cpio.gz") bytes, rv $(sha256sum "$W/stage/bin/rv" | cut -c1-12)"
}

do_restore_locked() {
	local force=${1:-${FORCE:-}}
	[ -e "$BK/manifest.txt" ] || { say "no backup: nothing to restore"; return 4; }
	if [ -z "$force" ] && [ -e "$BK/staged-manifest.txt" ]; then
		manifest > /tmp/vk20-now.$$
		if ! cmp -s /tmp/vk20-now.$$ "$BK/staged-manifest.txt"; then
			echo "stage-vm: the shared stage is not what was staged here (somebody staged or changed files since):" >&2
			diff "$BK/staged-manifest.txt" /tmp/vk20-now.$$ | head -12 >&2
			rm -f /tmp/vk20-now.$$
			say "FORCE=1 puts the backup back anyway (that discards their stage)"
			return 5
		fi
		rm -f /tmp/vk20-now.$$
	fi
	stop_servers
	rsync -a --delete "$BK/stage/" "$W/stage/"
	rsync -a --delete --exclude /disk/ "$BK/www/" "$W/www/"
	rsync -a --delete "$BK/runtime/" "$W/linux-wasm/runtime/"
	rsync -a --delete "$BK/guest-extra/" "$W/guest-extra/"
	manifest > /tmp/vk20-after.$$
	if ! cmp -s /tmp/vk20-after.$$ "$BK/manifest.txt"; then
		echo "stage-vm: the restored stage differs from the backup:" >&2
		diff "$BK/manifest.txt" /tmp/vk20-after.$$ | head -20 >&2
		rm -f /tmp/vk20-after.$$
		return 6
	fi
	rm -f /tmp/vk20-after.$$
	local n
	n=$(wc -l < "$BK/manifest.txt")
	rm -rf "$BK"
	say "restored: $n entries identical to the backup (files: content, mode, time; links; directories), backup removed"
}

do_restore() { lock; do_restore_locked; }

do_verify() {
	[ -e "$BK/manifest.txt" ] || die "no backup to compare with" 4
	manifest > /tmp/vk20-v.$$
	if cmp -s /tmp/vk20-v.$$ "$BK/manifest.txt"; then say "the shared stage is identical to the backup"; rm -f /tmp/vk20-v.$$; return 0; fi
	diff "$BK/manifest.txt" /tmp/vk20-v.$$ | head -20
	rm -f /tmp/vk20-v.$$
	return 1
}

do_status() {
	if [ -e "$BK/manifest.txt" ]; then
		say "STAGED. $(cat "$BK/info.txt" | tr '\n' ';')"
		[ -e "$BK/staged-manifest.txt" ] || say "(not sealed: a stage that did not finish)"
	else
		say "not staged: the shared stage is whatever it was (no backup in $BK)"
	fi
	say "initramfs $(ls -l --time-style='+%F %R' "$W/www/initramfs-wasm64_nommu.cpio.gz" | awk '{print $5, $6, $7}'), rv $(sha256sum "$W/stage/bin/rv" | cut -c1-12), rvgpu $(ls -l --time-style='+%F %R' "$W/www/rvgpu/rvgpu.wasm" 2> /dev/null | awk '{print $6, $7}')"
	say "listening: $(ss -ltn 2> /dev/null | awk 'NR>1 {print $4}' | grep -E ':(8000|8001|8002|8016|8019|8020)$' | tr '\n' ' ')"
	tmux list-sessions -F '#{session_name}' 2> /dev/null | grep '^vk20-' | tr '\n' ' ' | sed 's/^/stage-vm: sessions: /'; echo
}

stop_servers() {
	for s in vk20-site vk20-vdec-www vk20-matrix-www; do tmux kill-session -t "$s" 2> /dev/null || true; done
	rm -f "$W/www/disk/vk16"	# (a symlink of ours; the vk16 disk itself stays in ~/vk16)
}

serve_site() {
	local mode=${1:-vrend}
	case $mode in vrend|webgpu|auto) ;; *) die "serve: vrend, webgpu or auto" ;; esac
	mkdir -p "$HOME/vk20-dist"
	ln -sfn "$W/www" "$HOME/vk20-dist/www"
	ln -sfn "$W/www/disk" "$HOME/vk20-dist/disk"
	tmux kill-session -t vk20-site 2> /dev/null || true
	tmux new-session -d -s vk20-site "python3 $HOME/wt/$TREE/docker/serve.py --dist $HOME/vk20-dist --site /nonexistent --port 8002 --net-port 8001 --disk daily --gpu-exec $mode 2>&1 | tee $W/vk20-site.log"
	sleep 1
	for i in 1 2 3 4 5; do
		curl -s -m 3 localhost:8002/rv-site.json && { echo; say "site server on :8002, gpu_exec $mode"; return 0; }
		sleep 1
	done
	die "the site server on :8002 did not answer (tmux attach -t vk20-site)"
}

# A web server for a directory on the guest network (10.0.5.1), unless something already answers on the port.
serve_dir() { # session port dir probe-file
	if curl -s -m 3 -o /dev/null -w '%{size_download}' "http://10.0.5.1:$2/$4" | grep -qv '^0$'; then say "10.0.5.1:$2 already serves $4"; return 0; fi
	tmux kill-session -t "$1" 2> /dev/null || true
	tmux new-session -d -s "$1" "python3 -m http.server $2 --bind 10.0.5.1 --directory $3"
	sleep 1
	local n
	n=$(curl -s -m 5 "http://10.0.5.1:$2/$4" | wc -c)
	[ "$n" -gt 0 ] && say "10.0.5.1:$2 serves $4 ($n bytes), session $1" || say "WARNING: 10.0.5.1:$2 does not serve $4 ($3 missing it?)"
}

do_helpers() {
	ip -4 addr | grep -q 'inet 10.0.5.1/' || { say "WARNING: no 10.0.5.1 in the VM: the machine's network is not set up (sudo tools/net-setup.sh, and tools/net-relay.py running)"; return 0; }
	serve_dir vk20-vdec-www 8019 "$HOME/vdec/www" v.tgz
	serve_dir vk20-matrix-www 8020 "$HOME/vk17/www" m.tgz
}

do_vk16() {
	local d=$HOME/vk16/disk/vk16
	case ${1:-on} in
		on)
			[ -e "$d/manifest.json" ] || die "no derived disk vk16 in $d (VK-16 built it: release + apt blender; see the card)"
			ln -sfn "$d" "$W/www/disk/vk16"
			if ! ss -ltn | awk '{print $4}' | grep -q ':8016$'; then
				tmux new-session -d -s vk20-vk16-www "cd $HOME/vk16 && python3 -m http.server 8016 --bind 0.0.0.0"
				sleep 1
			fi
			say "disk vk16: $(curl -s -o /dev/null -w '%{http_code}' localhost:8000/disk/vk16/manifest.json) on :8000/disk/vk16, $(curl -s -o /dev/null -w '%{http_code}' localhost:8016/disk/vk16/manifest.json) on :8016" ;;
		off) rm -f "$W/www/disk/vk16"; tmux kill-session -t vk20-vk16-www 2> /dev/null || true; say "disk vk16 unlinked" ;;
	esac
}

case ${1:-} in
	backup) lock; backup ;;
	stage) do_stage ;;
	seal) lock; seal ;;
	restore) do_restore ;;
	verify) do_verify ;;
	status) do_status ;;
	serve) shift; serve_site "$@" ;;
	helpers) do_helpers ;;
	vk16) shift; do_vk16 "$@" ;;
	stop) lock; stop_servers; tmux kill-session -t vk20-vk16-www 2> /dev/null || true; say "stopped" ;;
	manifest) manifest ;;
	*) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;;
esac
