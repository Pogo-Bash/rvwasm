#!/bin/bash
# KCalc (a dylink main module, branch nqshared) on the shared Qt and KF6 libraries, under node-machine with dynamic libraries
# on, on the window server, with the library manifest preloaded; wl-dump drives it (WL_DUMP_ACTIONS) and takes PNGs.
#   [WL_DUMP_ACTIONS=...] [LDDEBUG=1] [MANIFEST_OFF=1] tests/wasmdyn/run-kcalc.sh [SECONDS]     PNGs in build/wasmdyn/kc-shots
# Needs: the build VM's sysroot-shared (nqshared: usr/bin/kcalc, the libraries, plugins) and out/ld-wasm, the dev server,
# disk "daily". The overlay is /usr/lib (the libraries by every file name they are needed under), /usr/lib/qt6/plugins,
# /wasmdyn/{ld-wasm,kcalc}, and links to the Debian disk's xkb data, fonts and icons. ONE node-machine at a time.
set -e
cd "$(dirname "$0")/../.."
B=build/wasmdyn
Q=$B/kc
SYSVM=~/rvwasm/nativeqt/sysroot-shared/usr
export PATH=${RVWASM_MAIN:-$HOME/dev/rvwasm}/tools/lima/bin:$PATH
SSH="ssh -F $HOME/.lima/rvwasm/ssh.config"
mkdir -p $Q
tests/wasmdyn/mkrt.sh
PLUGINS="platforms/libqwayland-generic.so wayland-shell-integration/libxdg-shell.so wayland-decoration-client/libbradient.so kiconthemes6/iconengines/KIconEnginePlugin.so iconengines/libqsvgicon.so imageformats/libqsvg.so imageformats/libqjpeg.so"
P=lib/qt6/plugins
LISTARGS=""
for p in $PLUGINS; do LISTARGS="$LISTARGS $P/$p"; done
$SSH lima-rvwasm "cd $SYSVM && python3 ~/wt/wasmdyn/tests/wasmdyn/closure.py lib bin/kcalc $LISTARGS" > $Q/needed.txt
rsync -a -e "$SSH" --files-from=$Q/needed.txt lima-rvwasm:$SYSVM/lib/ $Q/lib/
for p in $PLUGINS; do mkdir -p $Q/plugins/$(dirname $p); done
rsync -a -e "$SSH" --relative lima-rvwasm:$SYSVM/./bin/kcalc $Q/
for p in $PLUGINS; do rsync -a -e "$SSH" lima-rvwasm:$SYSVM/$P/$p $Q/plugins/$p; done
rsync -a -e "$SSH" lima-rvwasm:wt/wasmdyn/tests/wasmdyn/out/ld-wasm $Q/
python3 - <<'PY'
import os, shutil
Q = "build/wasmdyn/kc"
root = Q + "-root"
shutil.rmtree(root, ignore_errors=True)
os.makedirs(root + "/usr/lib/qt6/plugins")
os.makedirs(root + "/wasmdyn")
for n in open(Q + "/needed.txt").read().split():
    src = Q + "/lib/" + n
    if os.path.islink(src):
        os.symlink(os.readlink(src), root + "/usr/lib/" + n)
    else:
        shutil.copy(src, root + "/usr/lib/" + n)
for dirpath, _, files in os.walk(Q + "/plugins"):
    for f in files:
        rel = os.path.relpath(os.path.join(dirpath, f), Q + "/plugins")
        os.makedirs(os.path.dirname(root + "/usr/lib/qt6/plugins/" + rel), exist_ok=True)
        shutil.copy(os.path.join(dirpath, f), root + "/usr/lib/qt6/plugins/" + rel)
shutil.copy(Q + "/ld-wasm", root + "/wasmdyn/ld-wasm")
shutil.copy(Q + "/bin/kcalc", root + "/wasmdyn/kcalc")
for link, target in (("usr/share/X11", "/debian/usr/share/X11"), ("usr/share/fonts", "/debian/usr/share/fonts"),
                     ("usr/share/icons", "/debian/usr/share/icons"), ("etc/fonts", "/debian/etc/fonts")):
    os.makedirs(os.path.dirname(root + "/" + link), exist_ok=True)
    os.symlink(target, root + "/" + link)
print("overlay: %d library names" % len(open(Q + "/needed.txt").read().split()))
PY
(cd $Q-root && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2>/dev/null | gzip -1 > ../kc.cpio.gz)
ls -la $B/kc.cpio.gz

SECONDS_=${1:-20}
DL=$B/kc-dl
FP=$(cd $Q-root && find . -type f -exec stat -f '%N %z %m' {} + | sort | shasum | cut -d' ' -f1)
if [ "$(cat $DL/.fp 2> /dev/null)" != "$FP" ]; then
	rm -rf $DL
	python3 tools/dl-manifest.py --out $DL $(find $Q-root/usr/lib -type f \( -name '*.so*' \) | sort) | cut -c1-80 | tail -3
	echo "$FP" > $DL/.fp
fi
rm -rf $B/kc-shots; mkdir -p $B/kc-shots
PORT=$((20000 + RANDOM % 20000))
python3 -m http.server $PORT --bind 127.0.0.1 --directory $DL > /dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2> /dev/null' EXIT
sleep 1
export WL_DUMP_DIR=$B/kc-shots WL_DUMP_MAX=3 WL_DUMP_ACTIONS="${WL_DUMP_ACTIONS-windows:1,600|settle:3|shot:kcalc|click:185,404|click:392,308|click:288,404|click:392,436|settle:2|shot:2plus3|type:*4|key:Enter|settle:2|shot:times4}"
ENVS="HOME=/tmp XDG_CACHE_HOME=/tmp XDG_RUNTIME_DIR=/debian/tmp/runtime-root WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland QT_QPA_FONTDIR=/debian/usr/share/fonts/truetype/dejavu XDG_DATA_DIRS=/debian/usr/share:/usr/share ${LDDEBUG:+LD_DEBUG=1}"
CMD="cd /wasmdyn; debian 'desktop wayland sleep 900' > /dev/null 2>&1 & while [ ! -S /debian/tmp/runtime-root/wayland-0 ]; do sleep 0.5; done; echo SERVER-UP; export $ENVS; ./ld-wasm ./kcalc > /tmp/kcalc.out 2>&1 & KP=\$!; sleep $SECONDS_; grep -E 'VmRSS|VmSize|Threads' /proc/\$KP/status; ls /proc/\$KP/task; echo MAPS-BEGIN; cat /proc/\$KP/maps; echo MAPS-END; kill \$KP 2>/dev/null; wait \$KP; echo KCALC-RC=\$?; cat /tmp/kcalc.out; echo KCALC-DONE"
[ -n "$RUNCMD" ] && CMD="$RUNCMD"
MF=(); [ -z "$MANIFEST_OFF" ] && MF=(--dl-manifest http://127.0.0.1:$PORT/libs.json)
while pgrep -f "^node .*tools/[n]ode-machine.js" > /dev/null; do sleep 10; done
perl -e 'alarm shift; exec @ARGV' 700 node --experimental-wasm-memory64 tools/node-machine.js --runtime $B/rt \
	--kernel $B/vmlinux.wasm --initramfs $B/initramfs.cpio.gz --initramfs $B/kc.cpio.gz --disk daily \
	--timeout 640000 --query "?dl=1" --hook tests/wasmdyn/widgets-hook.js "${MF[@]}" "$CMD"
