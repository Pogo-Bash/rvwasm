#!/bin/bash
# Build the boot image that has a Debian riscv64 system in /debian, next to
# the usual one. Run inside the build VM, after tools/stage.sh.
#
#   build-debian-image.sh root       make ~/rvwasm/debian-root (once; needs sudo and the network)
#   build-debian-image.sh image      pack it into ~/rvwasm/www/initramfs-debian-<variant>.cpio.gz
#   build-debian-image.sh x-root     make ~/rvwasm/debian-root-x: a copy of the root with X installed
#   build-debian-image.sh image x    pack that into ~/rvwasm/www/initramfs-debian-x-<variant>.cpio.gz
#   build-debian-image.sh kde-root   make ~/rvwasm/debian-root-kde: a copy of the X root with KDE Plasma installed
#   build-debian-image.sh dev [x|kde]
#                                    add what it takes to work on that root: git, Node, Python, curl, ripgrep,
#                                    and Claude Code (see DEV_PACKAGES and CLAUDE_CODE below)
#   build-debian-image.sh chromium [x|kde]
#                                    add Chromium to that root, from RevyOS (see CHROMIUM_PACKAGES below)
#   build-debian-image.sh home [SIZE]
#                                    make the disk a person's own files go on: an empty ext4 file system
#                                    (default 16G), in ~/rvwasm/www/disk/home. A browser copies it once and
#                                    keeps its copy from then on (tools/www/disk-worker.js), so making it
#                                    again changes nothing for anyone who has one.
#   build-debian-image.sh release-root
#                                    make ~/rvwasm/debian-root-release, the release image's root (README.md
#                                    "Run it"): a copy of the KDE root after `dev kde` and `chromium kde` (what
#                                    the daily disk has) with RELEASE_PACKAGES, the native plasmashell from
#                                    NQ_BUNDLE (tools/nq-bundle.sh) in /opt/nq, the video driver of
#                                    gpu/vdec/driver, a test video, and /etc/rvwasm/autostart, which /init
#                                    starts at boot: the desktop. Then `disk release 8G`.
#   build-debian-image.sh disk [x|kde|release] [SIZE] [NAME]
#                                    make a disk of the root instead: an ext4 file system (default 2G), cut
#                                    into the pieces the page fetches as it needs them, in
#                                    ~/rvwasm/www/disk/<name> (dev.html?disk=debian, debian-x or debian-kde;
#                                    or NAME: a browser that has a disk keeps what was written to it only
#                                    until the image of that name is another one, so a new image under a
#                                    new name leaves the old one's users alone)
#
# The root is Debian trixie's "minbase" set of packages, installed and
# configured by debootstrap; its riscv64 maintainer scripts run under
# qemu-user-static (binfmt_misc with the F flag, so that it works in a chroot).
#
# The image is the usual initramfs with that root appended under /debian,
# minus what a small system can do without: downloaded packages and package
# lists (apt fetches them again), documentation, manual pages, translations.
# About 65 MB unpacked. On Linux/Wasm, `debian` enters it (tools/debian-enter).
#
# A disk has the whole root, nothing left out: what is never read is never
# fetched. The machine's /init mounts it on /debian.
#
# The X image also leaves out LLVM, Mesa's drivers and what they need
# (180 MB): the X server is linked against Mesa but uses none of it to draw
# on a frame buffer.

set -e

S="$(realpath "$(dirname "$0")/..")"
W="${RVWASM_WORK:-$HOME/rvwasm}"
: "${LW_VARIANT:=wasm64_nommu}"
ROOT="$W/debian-root"
NAME=debian
if [ "$2" = x ]; then
	ROOT="$W/debian-root-x"
	NAME=debian-x
elif [ "$2" = kde ]; then
	ROOT="$W/debian-root-kde"
	NAME=debian-kde
elif [ "$2" = release ]; then
	ROOT="$W/debian-root-release"
	NAME=release
fi

# What the X image has on top of the base system: the X server with its
# frame buffer and input device drivers, twm and a few small programs, and a
# light desktop: IceWM, a file manager, an editor, two web browsers (NetSurf
# and Dillo), a calculator, an image viewer, three games, fonts.
X_PACKAGES="xserver-xorg-core xserver-xorg-video-fbdev xserver-xorg-input-evdev xinit twm xterm
	x11-apps x11-xserver-utils xfonts-base
	icewm pcmanfm l3afpad netsurf-gtk dillo galculator gpicview xbill xmahjongg xdemineur fonts-dejavu-core
	librsvg2-common tango-icon-theme"
# KDE Plasma on X: the desktop shell, the window manager, a terminal, a file
# manager, the settings program, and the session bus they all talk over.
# (Too large for an image in memory: this root is for a disk.)
KDE_PACKAGES="plasma-desktop plasma-workspace kwin-x11 konsole dolphin systemsettings kate
	dbus dbus-x11 breeze breeze-icon-theme fonts-noto-core
	weston wayland-utils libwayland-dev wayland-protocols libxkbcommon-tools plasma-wayland-protocols wl-clipboard"
# (The last line: Wayland test clients (weston-simple-shm, weston-terminal, weston-flower, wayland-info)
# for the page's own window server (wlserver/), the protocol XML that its tables are made from, xkbcli (its
# keymap-us.xkb is `xkbcli compile-keymap --layout us`), and wl-copy/wl-paste to try its clipboard with.)
# For working on the machine itself: git, and what Claude needs to run here.
DEV_PACKAGES="git nodejs npm ripgrep curl ca-certificates python3 python3-httpx python3-pydantic less procps
	gcc g++ libc6-dev make openssh-client python3-venv sqlite3 htop nano vim-tiny file strace xdg-utils
	firefox-esr vim fastfetch fonts-hack fonts-noto-cjk kpackagetool6"
# (vim: the full one, next to vim-tiny; fonts-hack: Plasma's default fixed-width font, which Konsole asks
# for; fonts-noto-cjk: Chinese, Japanese and Korean text, boxes without it; kpackagetool6: what Plasma
# themes' install scripts use, e.g. Catppuccin's.)
# What the release image has on top of that (release-root): the sensors daemon the system monitor widgets ask
# (started by D-Bus on the first request), the sound server and its tools (tools/x/desktop starts PulseAudio
# if it is there), ALSA's tools, and a video player with VA-API (mpv, vainfo; libva-dev only for the headers
# the video driver is built against). Not plasma-pa: the native plasmashell has no build of its applet.
RELEASE_PACKAGES="ksystemstats pulseaudio pulseaudio-utils alsa-utils mpv vainfo libva-dev libva-drm2 libva-wayland2"
# A short test video for mpv (gpu/vdec/test/make-media.sh's H.264 1080p pattern), if there is one.
RELEASE_VIDEO="${RELEASE_VIDEO:-$HOME/vdec/media/h264-1080p.mp4}"
# Claude Code is a program for x86-64 and arm64 since 2.1.113; this is the
# last release that is JavaScript, for Node 18 or later. Of the package, only
# that script: the rest is programs for other processors.
CLAUDE_CODE=2.1.112
CLAUDE_CODE_SHA512=f4552027410ebc82f2848ab114a35596279b8948cbebc770a445b77864927b4be4543275739a8c0cd59cdb6816df3903ef3440aad7d03d21afd2de2f30cd833c
# Chromium. Debian does not build it for riscv64; RevyOS (ISCAS) rebuilds
# Debian trixie's own package for it, with a patch for the sandbox, and
# publishes it in an apt repository of its own. Only these packages come
# from there (an apt preference pins everything else from it out), and a
# package from there is taken over Debian's only when Debian has none.
# The repository's signing key is the one in RevyOS's keyring package; the
# copy that is fetched is checked against its hash here (the same file is
# in the mirror's revyos-keyring package and in RevyOS's git repository).
CHROMIUM_PACKAGES="chromium chromium-common chromium-sandbox chromium-driver chromium-headless-shell chromium-shell"
REVYOS_REPO="https://mirror.iscas.ac.cn/revyos/trixie/revyos-addons"
REVYOS_KEY_URL="https://raw.githubusercontent.com/revyos/revyos-keyring/master/keyrings/revyos-keyring.gpg"
REVYOS_KEY_SHA256=62dfba67d30e21c98a8e17633ce0d1c5ab0eea8ea9e6102f79cac56dda4cc3cb
IN_ROOT=(/usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive)

# The tools of tools/guest, into a root that has Python.
guest_tools() {
	sudo install -m 755 "$S/tools/guest/agent" "$ROOT/usr/local/bin/agent"
	sudo install -m 644 "$S/tools/guest/whereami.txt" "$ROOT/root/whereami.txt"
	if [ -e "$ROOT/usr/local/lib/node_modules/@anthropic-ai/claude-code/cli.js" ]; then
		sudo install -m 755 "$S/tools/guest/claude" "$ROOT/usr/local/bin/claude"
	fi
}

# Our configuration for X, into a root that has X (tools/x).
x_configuration() {
	sudo cp "$S/tools/x/xorg.conf" "$ROOT/etc/X11/xorg.conf"
	sudo install -m 755 "$S/tools/x/desktop" "$ROOT/usr/local/bin/desktop"
	sudo mkdir -p "$ROOT/root/.icewm"
	sudo cp "$S"/tools/x/icewm/* "$ROOT/root/.icewm/"
	sudo mkdir -p "$ROOT/etc/gtk-3.0"
	sudo cp "$S/tools/x/gtk-settings.ini" "$ROOT/etc/gtk-3.0/settings.ini"
	# Konsole's default profile for everybody (tools/x/xdg): Konsole's own defaults but for
	# bidirectional (right-to-left) text rendering, which is off. Profiles are looked for in
	# $XDG_DATA_DIRS/konsole, the default one is named in konsolerc, which KConfig reads from
	# /etc/xdg under the user's own: a user's konsolerc that names no profile keeps this one.
	if [ -d "$ROOT/usr/share/konsole" ] || [ -x "$ROOT/usr/bin/konsole" ]; then
		sudo mkdir -p "$ROOT/etc/xdg" "$ROOT/usr/local/share/konsole"
		sudo install -m 644 "$S/tools/x/xdg/config/konsolerc" "$ROOT/etc/xdg/konsolerc"
		sudo install -m 644 "$S/tools/x/xdg/data/konsole/Default.profile" "$ROOT/usr/local/share/konsole/Default.profile"
	fi
}

case "$1" in
root)
	sudo rm -rf "$ROOT"
	mkdir -p "$W/debian-debs"
	sudo debootstrap --arch=riscv64 --variant=minbase --cache-dir="$W/debian-debs" \
		--keyring=/usr/share/keyrings/debian-archive-keyring.gpg \
		trixie "$ROOT" http://deb.debian.org/debian
	;;
x-root)
	X="$W/debian-root-x"
	sudo rm -rf "$X"
	sudo cp -a "$W/debian-root" "$X"
	sudo cp /etc/resolv.conf "$X/etc/resolv.conf"
	sudo mount -t proc none "$X/proc"
	trap 'sudo umount "$X/proc"' EXIT
	sudo chroot "$X" "${IN_ROOT[@]}" apt-get update
	# shellcheck disable=SC2086
	sudo chroot "$X" "${IN_ROOT[@]}" apt-get install -y --no-install-recommends $X_PACKAGES
	sudo chroot "$X" "${IN_ROOT[@]}" apt-get clean
	;;
kde-root)
	K="$W/debian-root-kde"
	sudo rm -rf "$K"
	sudo cp -a "$W/debian-root-x" "$K"
	sudo cp /etc/resolv.conf "$K/etc/resolv.conf"
	sudo mount -t proc none "$K/proc"
	trap 'sudo umount "$K/proc"' EXIT
	sudo chroot "$K" "${IN_ROOT[@]}" apt-get update
	# shellcheck disable=SC2086
	sudo chroot "$K" "${IN_ROOT[@]}" apt-get install -y --no-install-recommends $KDE_PACKAGES
	sudo chroot "$K" "${IN_ROOT[@]}" apt-get clean
	;;
dev)
	sudo cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
	sudo mount -t proc none "$ROOT/proc"
	trap 'sudo umount "$ROOT/proc"' EXIT
	sudo chroot "$ROOT" "${IN_ROOT[@]}" apt-get update
	# shellcheck disable=SC2086
	sudo chroot "$ROOT" "${IN_ROOT[@]}" apt-get install -y --no-install-recommends $DEV_PACKAGES
	sudo chroot "$ROOT" "${IN_ROOT[@]}" apt-get clean
	# From the npm registry, and only if it is the file the registry's index
	# says it is (dist.integrity of that version, here in hexadecimal).
	mkdir -p "$W/dl"
	package="$W/dl/claude-code-$CLAUDE_CODE.tgz"
	[ -e "$package" ] ||
		curl -sfL -o "$package" "https://registry.npmjs.org/@anthropic-ai/claude-code/-/claude-code-$CLAUDE_CODE.tgz"
	echo "$CLAUDE_CODE_SHA512  $package" | sha512sum -c -
	to="$ROOT/usr/local/lib/node_modules/@anthropic-ai/claude-code"
	sudo mkdir -p "$to"
	sudo tar xzf "$package" -C "$to" --strip-components=1 --no-same-owner \
		package/cli.js package/package.json package/LICENSE.md package/README.md
	guest_tools
	;;
chromium)
	mkdir -p "$W/dl"
	key="$W/dl/revyos-keyring.gpg"
	[ -e "$key" ] || curl -sfL -o "$key" "$REVYOS_KEY_URL"
	echo "$REVYOS_KEY_SHA256  $key" | sha256sum -c -
	sudo install -m 644 "$key" "$ROOT/usr/share/keyrings/revyos-keyring.gpg"
	sudo tee "$ROOT/etc/apt/sources.list.d/revyos-addons.sources" > /dev/null <<- EOF
		Types: deb
		URIs: $REVYOS_REPO
		Suites: trixie
		Components: main
		Signed-By: /usr/share/keyrings/revyos-keyring.gpg
	EOF
	# Nothing from RevyOS but Chromium: the rest of its repository is left
	# at a priority that never installs.
	sudo tee "$ROOT/etc/apt/preferences.d/revyos-addons" > /dev/null <<- EOF
		Package: $CHROMIUM_PACKAGES chromium-l10n
		Pin: release o=revyos-addons
		Pin-Priority: 500

		Package: *
		Pin: release o=revyos-addons
		Pin-Priority: -1
	EOF
	sudo cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
	sudo mount -t proc none "$ROOT/proc"
	trap 'sudo umount "$ROOT/proc"' EXIT
	sudo chroot "$ROOT" "${IN_ROOT[@]}" apt-get update
	# shellcheck disable=SC2086
	sudo chroot "$ROOT" "${IN_ROOT[@]}" apt-get install -y --no-install-recommends $CHROMIUM_PACKAGES
	sudo chroot "$ROOT" "${IN_ROOT[@]}" apt-get clean
	;;
release-root)
	R="$W/debian-root-release"
	K="$W/debian-root-kde"
	NQ="${NQ_BUNDLE:-$W/nq-bundle}"
	[ -x "$K/usr/bin/node" ] && [ -x "$K/usr/lib/chromium/chromium" ] ||
		{ echo "release-root: $K needs kde-root, dev kde and chromium kde first" >&2; exit 1; }
	[ -x "$NQ/plasmashell" ] ||
		{ echo "release-root: no native plasmashell in $NQ (tools/nq-bundle.sh; NQ_BUNDLE says where)" >&2; exit 1; }
	sudo rm -rf "$R"
	sudo cp -a "$K" "$R"
	sudo cp /etc/resolv.conf "$R/etc/resolv.conf"
	sudo mount -t proc none "$R/proc"
	trap 'sudo umount "$R/proc"' EXIT
	sudo chroot "$R" "${IN_ROOT[@]}" apt-get update
	# shellcheck disable=SC2086
	sudo chroot "$R" "${IN_ROOT[@]}" apt-get install -y --no-install-recommends $RELEASE_PACKAGES
	sudo chroot "$R" "${IN_ROOT[@]}" apt-get clean
	sudo rm -rf "$R"/var/lib/apt/lists/*
	sudo umount "$R/proc"
	trap - EXIT
	# The native plasmashell and the QML packages, icons and overrides it brings (desktop wayland plasma-native
	# looks in /opt/nq when nothing was fetched into /run/nq).
	sudo rm -rf "$R/opt/nq"
	sudo mkdir -p "$R/opt/nq"
	sudo cp -r "$NQ/." "$R/opt/nq/"
	sudo chown -R 0:0 "$R/opt/nq"
	sudo chmod 755 "$R/opt/nq/plasmashell"
	# The video driver (gpu/vdec: VA-API to the page's WebCodecs), where libva looks for drivers; tools/x/desktop
	# names it (LIBVA_DRIVER_NAME=rvwasm) for programs on the GPU device. One built with the machine
	# (/run/rvgpu/va, tools/stage.sh) is taken before it.
	# (Against libva's headers only: the root's C library headers are another glibc than the cross compiler's.)
	tmp="$(mktemp -d)"
	mkdir -p "$tmp/include"
	cp -r "$R/usr/include/va" "$tmp/include/va"
	VA_INC="$tmp/include" "$S/gpu/vdec/driver/build.sh" "$tmp" > /dev/null
	sudo install -m 644 "$tmp/rvwasm_drv_video.so" "$R/usr/lib/riscv64-linux-gnu/dri/rvwasm_drv_video.so"
	rm -rf "$tmp"
	if [ -f "$RELEASE_VIDEO" ]; then
		sudo mkdir -p "$R/usr/local/share/rvwasm"
		sudo install -m 644 "$RELEASE_VIDEO" "$R/usr/local/share/rvwasm/test-h264-1080p.mp4"
	fi
	# A first start with nothing left of anybody's: no configuration or caches of the desktop in root's home
	# (Plasma then writes its default layout, the one in /opt/nq/share: no Discover launcher).
	sudo rm -rf "$R/root/.config" "$R/root/.cache" "$R/root/.local" "$R/root/.bash_history" "$R/tmp/"*
	# Claude Code (dev) is proprietary and is not to be redistributed: the release is a public download, so it has
	# none of it, nor the wrapper, nor anything of Claude's in anyone's home (the private dev image keeps all of it).
	# (`agent`, tools/guest/agent, is our own program and stays.)
	sudo rm -rf "$R/usr/local/lib/node_modules/@anthropic-ai" "$R/usr/lib/node_modules/@anthropic-ai" \
		"$R/usr/local/bin/claude" "$R/usr/bin/claude" "$R/root/.claude" "$R/root/.claude.json" "$R/root/.npm" \
		"$R/root/.ssh" "$R/root/.gitconfig" "$R/root/.python_history" "$R/root/.lesshst" "$R/root/.viminfo"
	sudo find "$R/home" -mindepth 1 -delete 2> /dev/null || true
	if sudo find "$R" -xdev \( -iname '*claude-code*' -o -name claude -o -name '.claude*' \) 2> /dev/null | grep -q .; then
		echo "release-root: Claude Code is still in the release root" >&2
		exit 1
	fi
	# mpv with the page's GPU and video decoder when the desktop has them (tools/x/mpv), and its defaults: VA-API, and
	# OpenGL ES 3.0 (GLSL ES 3.00) rather than virgl's desktop OpenGL 2.1, whose GLSL 1.20 mpv calls too old.
	sudo install -m 755 "$S/tools/x/mpv" "$R/usr/local/bin/mpv"
	sudo install -m 755 "$S/tools/x/rv-video-check" "$R/usr/local/bin/rv-video-check"
	sudo mkdir -p "$R/etc/mpv"
	printf '# rvwasm (tools/build-debian-image.sh release-root)\nhwdec=vaapi\nopengl-es=yes\n' | sudo tee "$R/etc/mpv/mpv.conf" > /dev/null
	# Chromium started from Plasma's launcher or by another program goes through Debian's /usr/bin/chromium, which
	# reads /etc/chromium.d: the flags it needs here (tools/x/desktop gives the same to `desktop wayland chromium`,
	# which starts it directly): Wayland, no sandbox (root, no namespaces), one process for the GPU, no GPU.
	sudo tee "$R/etc/chromium.d/rvwasm" > /dev/null <<- 'EOF'
		# rvwasm (tools/build-debian-image.sh release-root): Chromium as it runs on this machine; see tools/x/desktop.
		export CHROMIUM_FLAGS="$CHROMIUM_FLAGS --ozone-platform=wayland --no-sandbox --test-type --no-zygote --in-process-gpu --disable-gpu --disable-dev-shm-usage --renderer-process-limit=1 --disable-site-isolation-trials --disable-features=AudioServiceOutOfProcess,OptimizationGuideOnDeviceModel,OnDeviceModelService --disable-component-update --no-first-run --disable-breakpad --disable-crash-reporter --disable-sync"
	EOF
	# The desktop at boot (tools/initramfs-init runs this unless the page says ?autostart=0).
	sudo mkdir -p "$R/etc/rvwasm"
	sudo tee "$R/etc/rvwasm/autostart" > /dev/null <<- 'EOF'
		#!/bin/bash
		# rvwasm-autostart: the desktop (Plasma's shell, native, on the page's window server, with sound)
		# Started by the machine's /init at boot (tools/initramfs-init in the rvwasm source tree) unless the page
		# was opened with ?autostart=0. What it prints: /root/autostart.log; the window server's: /root/wlserver.log.
		exec > /root/autostart.log 2>&1 < /dev/null
		echo "autostart: $(date)"
		exec desktop wayland plasma-native
	EOF
	sudo chmod 755 "$R/etc/rvwasm/autostart"
	echo "release root: $(sudo du -sh "$R" | cut -f1); native plasmashell: $(cat "$R/opt/nq/info.txt" 2> /dev/null)"
	;;
image)
	base="$W/www/initramfs-$LW_VARIANT.cpio.gz"
	out="$W/www/initramfs-$NAME-$LW_VARIANT.cpio.gz"
	if [ -d "$ROOT/etc/X11" ]; then
		x_configuration
	fi
	tmp="$(mktemp -d)"
	trap 'sudo rm -rf "$tmp"' EXIT

	gzip -dc "$base" > "$tmp/initramfs.cpio"
	# First /debian itself and the directories whose contents are left out
	# below, empty: an archive has to name a directory before what is in it.
	mkdir -p "$tmp/empty"
	(
		cd "$tmp/empty"
		mkdir -p debian/proc debian/sys debian/dev debian/run debian/var/cache/apt/archives/partial \
			debian/var/lib/apt/lists/partial debian/usr/share/doc debian/usr/share/man \
			debian/usr/share/locale debian/usr/share/info debian/var/log
		find debian -print0 |
			cpio --null -o --format=newc -A -O "$tmp/initramfs.cpio" --owner=0:0 2> /dev/null
	)
	# Then the root. The archive names files relative to where cpio runs, and
	# they have to start with "debian/": a link of that name to the root.
	mkdir "$tmp/tree"
	ln -s "$ROOT" "$tmp/tree/debian"
	(
		cd "$tmp/tree"
		# As root: some of the tree is readable by root only. Device nodes
		# and the kernel's file systems are made when the system is entered.
		sudo find debian/ -mindepth 1 \
			\( -path debian/proc -o -path debian/sys -o -path debian/dev -o -path debian/run \
			   -o -path debian/var/cache/apt -o -path debian/var/lib/apt/lists \
			   -o -path debian/usr/share/doc -o -path debian/usr/share/man \
			   -o -path debian/usr/share/locale -o -path debian/usr/share/info \
			   -o -path debian/var/log -o -name 'libLLVM*' -o -name 'libgallium*' -o -name 'libz3*' -o -path '*/dri/*' \) -prune -o -print0 |
			sudo cpio --null -o --format=newc -A -O "$tmp/initramfs.cpio" 2> /dev/null
	)
	gzip -c "$tmp/initramfs.cpio" > "$out.tmp"
	mv "$out.tmp" "$out"
	ls -la "$tmp/initramfs.cpio" "$out"
	;;
disk)
	size="${3:-2G}"
	[ "$2" = x ] || [ "$2" = kde ] || [ "$2" = release ] || size="${2:-2G}"
	NAME="${4:-$NAME}"
	if [ "$NAME" = release ] && [ -e "$ROOT/usr/local/lib/node_modules/@anthropic-ai" ]; then
		echo "disk: Claude Code is in $ROOT; the release disk must not have it (step release-root purges it)" >&2
		exit 1
	fi
	if [ -d "$ROOT/etc/X11" ]; then
		x_configuration
	fi
	if [ -x "$ROOT/usr/bin/python3" ]; then
		guest_tools
	fi
	# fontconfig's caches name the time of the directory they describe,
	# to the nanosecond. mke2fs -d copies times to the second only, so on
	# the disk every cache would be out of date and every program that uses
	# fonts would scan all of them again at its start (Chromium spent a
	# minute on it). So the font directories' times are cut to the second
	# first, and the caches made for those times.
	if [ -x "$ROOT/usr/bin/fc-cache" ]; then
		for d in "$ROOT/usr/share/fonts" "$ROOT/usr/local/share/fonts" "$ROOT/usr/share/X11/fonts"; do
			[ -d "$d" ] || continue
			sudo find "$d" -type d -exec sh -c 'for x; do touch -c -m -d "@$(stat -c %Y "$x")" "$x"; done' sh {} +
		done
		sudo mount -t proc none "$ROOT/proc"
		sudo chroot "$ROOT" "${IN_ROOT[@]}" fc-cache -f -s
		sudo umount "$ROOT/proc"
	fi
	mkdir -p "$W/disks"
	image="$W/disks/$NAME.ext4"
	rm -f "$image"
	# As root: the files keep their owners. No reserved blocks; inode tables
	# and journal written out now, so that the first mount has nothing to
	# initialise (which would be written to the browser's storage).
	sudo mke2fs -q -t ext4 -L debian -m 0 -E lazy_itable_init=0,lazy_journal_init=0 -d "$ROOT" "$image" "$size"
	sudo chown "$USER" "$image"
	python3 "$S/tools/split-disk.py" "$image" "$W/www/disk/$NAME"
	;;
home)
	mkdir -p "$W/disks"
	image="$W/disks/home.ext4"
	rm -f "$image"
	# Everything written out now: a browser fetches all of it once, and nothing is left for the first mount
	# to initialise. One inode per 32 KiB: source trees are many small files.
	mke2fs -q -t ext4 -L home -m 0 -i 32768 -J size=16 -E lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0 \
		"$image" "${2:-16G}"
	python3 "$S/tools/split-disk.py" "$image" "$W/www/disk/home"
	du -sh "$W/www/disk/home"
	;;
*)
	echo "usage: $0 root|x-root|kde-root|dev [x|kde]|chromium [x|kde]|release-root|image [x]|home [SIZE]|disk [x|kde|release] [SIZE] [NAME]" >&2
	exit 2
	;;
esac
