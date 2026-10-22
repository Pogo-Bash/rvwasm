#!/bin/bash
# QtWidgets (qtwidgets-hello) on the shared Qt libraries, under node-machine with dynamic libraries on, on the window
# server (wlserver, in the Debian disk's chroot as `desktop wayland` starts it), a PNG of the window by tools/wl-dump.js.
#   tests/wasmdyn/run-widgets.sh [SECONDS] [LD_DEBUG]      PNGs in build/wasmdyn/shots, statistics at the end
# Needs: build.sh and qtwidgets-hello/build.sh in the build VM (out/), the nqshared sysroot there, the dev server (kernel,
# boot image, disk "daily"). The libraries go to /usr/lib and the plugins to /usr/lib/qt6/plugins of the machine's
# root file system (NEEDED entries are file names); MANIFEST=0 leaves the manifest preload out. ONE node-machine at a time.
set -e
cd "$(dirname "$0")/../.."
B=build/wasmdyn
Q=$B/qtw
SYS=lima-rvwasm:rvwasm/nativeqt/sysroot-shared/usr/lib
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
SSH="ssh -F $HOME/.lima/rvwasm/ssh.config"
mkdir -p $Q
tests/wasmdyn/mkrt.sh
rsync -a -e "$SSH" --include='*/' --include='lib*.so*' --exclude='*' $SYS/ $Q/usr/lib/
rsync -a -e "$SSH" lima-rvwasm:wt/wasmdyn/tests/wasmdyn/out/{ld-wasm,qtwidgets-hello,xkbtest} $Q/
# What the program needs: the closure of NEEDED entries over the program, the platform plugin, the shell integration and
# the decoration plugin (by file name, in /usr/lib).
python3 - <<'PY'
import os, shutil, sys
sys.path.insert(0, "tests/wasmdyn")
Q = "build/wasmdyn/qtw"
exec(open("tests/wasmdyn/needed.py").read().split("if __name__")[0])
lib = Q + "/usr/lib"
plugins = ["platforms/libqwayland-generic.so", "wayland-shell-integration/libxdg-shell.so", "wayland-decoration-client/libbradient.so"]
root = Q + "-root"
shutil.rmtree(root, ignore_errors=True)
os.makedirs(root + "/usr/lib/qt6/plugins", exist_ok=True)
os.makedirs(root + "/wasmdyn", exist_ok=True)
seen = set()
def add(n):
    """The library file name n in /usr/lib: a symlink stays one (the loader knows a file by its inode), its target is added too."""
    if n in seen:
        return
    seen.add(n)
    p = os.path.join(lib, n)
    if not os.path.lexists(p):
        sys.exit("missing library " + n)
    if os.path.islink(p):
        t = os.readlink(p)
        os.symlink(t, root + "/usr/lib/" + n)
        add(t)
        return
    shutil.copy(p, root + "/usr/lib/" + n)
    for m in needed(p):
        add(m)
for p in plugins:
    os.makedirs(os.path.dirname(root + "/usr/lib/qt6/plugins/" + p), exist_ok=True)
    shutil.copy(lib + "/qt6/plugins/" + p, root + "/usr/lib/qt6/plugins/" + p)
for f in [Q + "/qtwidgets-hello"] + [lib + "/qt6/plugins/" + p for p in plugins]:
    for n in needed(f):
        add(n)
files = seen
shutil.copy(Q + "/ld-wasm", root + "/wasmdyn/ld-wasm")
shutil.copy(Q + "/qtwidgets-hello", root + "/wasmdyn/qtwidgets-hello")
shutil.copy(Q + "/xkbtest", root + "/wasmdyn/xkbtest")
# What the program reads from the Debian system the window server runs in (the disk is mounted at /debian): xkb's data
# (xkb_context_new fails without /usr/share/X11/xkb), fonts and their configuration.
for link, target in (("usr/share/X11", "/debian/usr/share/X11"), ("usr/share/fonts", "/debian/usr/share/fonts"), ("etc/fonts", "/debian/etc/fonts")):
    os.makedirs(os.path.dirname(root + "/" + link), exist_ok=True)
    os.symlink(target, root + "/" + link)
shutil.copy("wlserver/keymaps/us.xkb", root + "/wasmdyn/us.xkb")
print("overlay: %d libraries, %d plugins" % (len(files), len(plugins)))
PY
(cd $Q-root && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2>/dev/null | gzip -1 > ../qtw.cpio.gz)
ls -la $Q.cpio.gz 2> /dev/null || ls -la $B/qtw.cpio.gz

# The manifest of every real file (symlinks have the content of their target): the libraries and plugins compiled ahead
# by compileStreaming, so no Worker compiles them. (tools/dl-manifest.py hashes in Python: slow, so only when changed.)
SECONDS_=${1:-8}
DL=$B/qtw-dl
FP=$(cd $Q-root && find . -type f -exec stat -f '%N %z %m' {} + | sort | shasum | cut -d' ' -f1)
if [ "$(cat $DL/.fp 2> /dev/null)" != "$FP" ]; then
	rm -rf $DL
	python3 tools/dl-manifest.py --out $DL $(find $Q-root/usr/lib -type f -name '*.so*' | sort) | cut -c1-80
	echo "$FP" > $DL/.fp
fi
rm -rf $B/shots; mkdir -p $B/shots
PORT=$((20000 + RANDOM % 20000))
python3 -m http.server $PORT --bind 127.0.0.1 --directory $DL > /dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2> /dev/null' EXIT
sleep 1
export WL_DUMP_DIR=$B/shots WL_DUMP_MAX=3 WL_DUMP_ACTIONS='windows:1,600|settle:3|shot:screen'
ENVS="HOME=/tmp XDG_CACHE_HOME=/tmp XDG_RUNTIME_DIR=/debian/tmp/runtime-root WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland QT_QPA_FONTDIR=/debian/usr/share/fonts/truetype/dejavu ${2:+LD_DEBUG=1}"
CMD="cd /wasmdyn; debian 'desktop wayland sleep 900' > /dev/null 2>&1 & while [ ! -S /debian/tmp/runtime-root/wayland-0 ]; do sleep 0.5; done; echo SERVER-UP; export $ENVS; for r in $(seq -s " " 1 ${RUNS:-1}); do ./ld-wasm ./qtwidgets-hello $SECONDS_; echo PROGRAM-RC=\$?; done"
MF=(); [ -z "$MANIFEST_OFF" ] && MF=(--dl-manifest http://127.0.0.1:$PORT/libs.json)
[ -n "$RUNCMD" ] && CMD="$RUNCMD"
while pgrep -f "^node .*tools/[n]ode-machine.js" > /dev/null; do sleep 10; done
perl -e 'alarm shift; exec @ARGV' 600 node --experimental-wasm-memory64 tools/node-machine.js --runtime $B/rt \
	--kernel $B/vmlinux.wasm --initramfs $B/initramfs.cpio.gz --initramfs $B/qtw.cpio.gz --disk daily \
	--timeout 540000 --query "?dl=1" --hook tests/wasmdyn/widgets-hook.js "${MF[@]}" "$CMD"
