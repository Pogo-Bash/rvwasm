#!/bin/bash
# The `nqs` disk (docs: Board/dynlink ship-plan section 2, card DL-02): a read-only ext4 image of the shared Qt/KF6 closure
# of KCalc and Konsole, mounted at /opt/nq-shared by the guest, in the pieces tools/split-disk.py writes.
#
#   shared/build-nqs-image.sh            (in the build VM; nothing is written outside $OUT and $PIECES)
#
#   SYS     sysroot-shared/usr            (default ~/rvwasm/nativeqt/sysroot-shared/usr)
#   LDWASM  the dynamic linker             (default ~/wt/wasmdyn/tests/wasmdyn/out/ld-wasm)
#   OUT     work directory                 (default ~/rvwasm/nqs): root/ (the tree), nqs.ext4, dl/ (library manifest), files.txt
#   NQS_REV the repo revision recorded in share/nqs/info.txt (default: git HEAD of the tree, else "unknown")
#   NATIVEPERL the native perl tree (branch nativeperl: tools/build-nativeperl.sh, tools/nativeperl-pack.sh), copied to
#           /opt/nq-shared/perl (default ~/rvwasm/nativeperl/pack/usr/lib/nativeperl; left out if it is not there)
#   PIECES  where the pieces go            (default ~/rvwasm/www/disk/nqs, the dev server's disk/nqs/)
#
# The closure is computed (tests/wasmdyn/closure.py: NEEDED entries, transitively) from the programs, the KIO helpers and the
# plugins one Konsole/KCalc session loads: nothing else of the 273 MB usr/lib is copied, nothing is stripped. The image is
# reproducible (fixed time, UUID, hash seed, owners, modes, no journal): the same inputs give the same bytes, and so the same
# manifest id (split-disk.py: the sha256 of the image).
#
# Layout (mount point /opt/nq-shared):
#   lib/ld-wasm  bin/nqs-run  bin/nqs-shell  etc/fonts/fonts.conf  share/applications/org.kde.{kcalc,konsole}.native.desktop
#   usr/bin/{kcalc,konsole}  usr/lib/*.so*  usr/lib/qt6/plugins/...  usr/lib/libexec/kf6/{kioworker,kiod6}
#   usr/share/{icu/76.1/icudt76l.dat,mime,knotifications6}  share/nqs/{info.txt,files.txt,maps.list}
#   perl/{bin/perl,lib/perl5/5.40.1,overrides}   (the native perl, 30 MB: tools/debian-enter binds it over /usr/bin/perl in the Debian root)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SYS=${SYS:-$HOME/rvwasm/nativeqt/sysroot-shared/usr}
LDWASM=${LDWASM:-$HOME/wt/wasmdyn/tests/wasmdyn/out/ld-wasm}
OUT=$HOME/rvwasm/nqs
PIECES=$HOME/rvwasm/www/disk/nqs
EPOCH=1760000000
export E2FSPROGS_FAKE_TIME=$EPOCH

PROGS="bin/kcalc bin/konsole lib/libexec/kf6/kioworker lib/libexec/kf6/kiod6"
# What a KCalc or Konsole session dlopens (native/shared/README.md, tests/wasmdyn/konsole-env.sh): the Wayland platform and its
# shell/decoration, the compose input context, the icon engines and image formats, the KIO worker plugins, Konsole's plugins.
PLUGINS="platforms/libqwayland-generic.so wayland-shell-integration/libxdg-shell.so wayland-decoration-client/libbradient.so
 platforminputcontexts/libcomposeplatforminputcontextplugin.so iconengines/libqsvgicon.so imageformats/libqsvg.so
 imageformats/libqjpeg.so kiconthemes6/iconengines/KIconEnginePlugin.so kf6/kio/kio_file.so kf6/kio/kio_trash.so
 kf6/kiod/kioexecd.so konsoleplugins/konsole_quickcommandsplugin.so konsoleplugins/konsole_sshmanagerplugin.so"
SHARE="icu mime knotifications6"
# Beyond KCalc/Konsole (an app's file nqs/apps/NAME.env, selected by NQS_APP=NAME, sets any of these; it also sets OUT and PIECES
# so that it does not overwrite the KCalc/Konsole image):
#   QMLDIRS   directories of lib/qt6/qml that get their qmldir and plugin .so only (not their sub-directories)
#   QMLTREES  directories of lib/qt6/qml that get every qmldir and .so below them
#   SHARE_FILES  files and directories below usr/share to copy (tar), besides SHARE
#   LAUNCHERS    desktop files from nqs/ for share/applications
#   PRELOAD_PROGS the programs whose closure is preloaded at page load
QMLDIRS=""; QMLTREES=""; SHARE_FILES=""
LAUNCHERS="org.kde.kcalc.native.desktop org.kde.konsole.native.desktop"
PRELOAD_PROGS="bin/kcalc lib/qt6/plugins/platforms/libqwayland-generic.so"
if [ -n "${NQS_APP:-}" ]; then . "$HERE/nqs/apps/$NQS_APP.env"; fi
# The KDE platform theme (plasma-integration) goes into every image: icons, fonts, colours (nqs-run sets QT_QPA_PLATFORMTHEME=KDE)
if [ -f "$SYS/lib/qt6/plugins/platformthemes/KDEPlasmaPlatformTheme6.so" ]; then
	PLUGINS="$PLUGINS platformthemes/KDEPlasmaPlatformTheme6.so"
	[ -n "${PRELOAD_SELF:-}" ] && PRELOAD_SELF="$PRELOAD_SELF lib/qt6/plugins/platformthemes/KDEPlasmaPlatformTheme6.so"
fi
# and the Breeze widget style (breeze's kstyle: the platform theme picks it by kdeglobals [KDE] widgetStyle, default Breeze)
if [ -f "$SYS/lib/qt6/plugins/styles/breeze6.so" ]; then
	PLUGINS="$PLUGINS styles/breeze6.so"
	[ -n "${PRELOAD_SELF:-}" ] && PRELOAD_SELF="$PRELOAD_SELF lib/qt6/plugins/styles/breeze6.so"
fi
# and Qt's TLS backend (OpenSSL linked into the plugin: QSslSocket, so every https request of QNetworkAccessManager: KNewStuff's
# "Get New ..." lists, previews and downloads; without it supportsSsl() is false, no backend is available and the request fails
# before it reaches the network). The CA certificates come from the Debian disk's /etc/ssl/certs, which the apps run beside (nqs-run).
if [ -f "$SYS/lib/qt6/plugins/tls/libqopensslbackend.so" ]; then
	PLUGINS="$PLUGINS tls/libqopensslbackend.so"
else
	echo "build-nqs-image: no tls/libqopensslbackend.so in $SYS/lib/qt6/plugins: the image would have no https" >&2
	exit 1
fi

[ -d "$SYS/lib" ] || { echo "build-nqs-image: no $SYS/lib" >&2; exit 1; }
[ -f "$LDWASM" ] || { echo "build-nqs-image: no $LDWASM" >&2; exit 1; }
R=$OUT/root
rm -rf "$R"
mkdir -p "$R"/{lib,bin,etc/fonts,share/applications,share/nqs,usr/bin,usr/lib/qt6/plugins,usr/lib/qt6/qml,usr/lib/libexec/kf6,usr/share}

# The closure.
list=()
for p in $PLUGINS; do list+=("lib/qt6/plugins/$p"); done
# QML modules: qmldir and plugin only (QMLDIRS); everything below QMLTREES (the QML files are in the plugin's resources; "prefer :/qt/qml/..." in the qmldir)
qmlfiles=()
for d in $QMLDIRS; do
	for f in "$SYS/lib/qt6/qml/$d"/qmldir "$SYS/lib/qt6/qml/$d"/*.so; do [ -f "$f" ] && qmlfiles+=("lib/qt6/qml/$d/$(basename "$f")"); done
done
for d in $QMLTREES; do
	# (all files: Kirigami's style selector loads components by file name, not from the module's resources)
	while read -r f; do qmlfiles+=("${f#$SYS/}"); done < <(find "$SYS/lib/qt6/qml/$d" -type f ! -name '*.qmltypes' ! -name '*.prl' ! -name '*.a' | sort)
done
for p in "${qmlfiles[@]}"; do case $p in *.so) list+=("$p") ;; esac; done
(cd "$SYS" && python3 "$REPO/tests/wasmdyn/closure.py" lib $PROGS "${list[@]}") > "$OUT/needed.txt"
(cd "$SYS/lib" && tar -cf - --files-from="$OUT/needed.txt") | tar -C "$R/usr/lib" -xf -
for p in "${list[@]}"; do
	mkdir -p "$R/usr/$(dirname "$p")"
	install -m 755 "$SYS/$p" "$R/usr/$p"
done
for p in $PROGS; do
	mkdir -p "$R/usr/$(dirname "$p")"
	install -m 755 "$SYS/$p" "$R/usr/$p"
done
# KIO execs `kioworker` (and D-Bus activation `kiod6`) directly, and a dylink main module needs ld-wasm (the kernel has no
# interpreter support yet: an exec of one fails at instantiate, "GOT.mem: module is not an object"): the program becomes NAME.bin and
# NAME a shell script that runs it through ld-wasm (the environment is inherited from the app).
for h in kioworker kiod6; do
	f=$R/usr/lib/libexec/kf6/$h
	[ -f "$f" ] || continue
	mv "$f" "$f.bin"
	printf '#!/bin/sh\nNQS=${NQS:-/opt/nq-shared}\nexec $NQS/lib/ld-wasm $NQS/usr/lib/libexec/kf6/%s.bin "$@"\n' "$h" > "$f"
	chmod 755 "$f"
done
# KPackage's "kns://" dependency handler (nqs-run points KPACKAGE_DEP_RESOLVERS_PATH here): a script that runs Debian's.
mkdir -p "$R/usr/lib/libexec/kf6/kpackagehandlers"
install -m 755 "$HERE/nqs/knshandler" "$R/usr/lib/libexec/kf6/kpackagehandlers/knshandler"
for d in $SHARE $SHARE_FILES; do
	(cd "$SYS/share" && tar -cf - "$d") | tar -C "$R/usr/share" -xf -
done
for p in "${qmlfiles[@]}"; do
	mkdir -p "$R/usr/$(dirname "$p")"
	install -m 755 "$SYS/$p" "$R/usr/$p"
done
# ICU: the data file only (the library's own tables are in libicuuc).
find "$R/usr/share/icu" -type f ! -name '*.dat' -delete
rmdir "$R"/usr/share/icu/* 2> /dev/null || true

# The dynamic linker and the guest side.
install -m 755 "$LDWASM" "$R/lib/ld-wasm"
install -m 755 "$HERE/nqs/nqs-run" "$HERE/nqs/nqs-shell" "$R/bin/"
install -m 644 "$HERE/nqs/fonts.conf" "$R/etc/fonts/fonts.conf"
# The programs' desktop files under the IDs of the emulated ones they replace (nqs/mkdesktop.py): Debian's own files where the
# build VM's release root has them (the exact IDs, names, icons, mime types), the upstream ones installed with the build otherwise;
# only the command changes. The desktop puts this share before Debian's in XDG_DATA_DIRS (tools/x/desktop).
NATIVE_APPS=$(for l in $LAUNCHERS; do l=${l#org.kde.}; echo ${l%.native.desktop}; done)
python3 "$HERE/nqs/mkdesktop.py" "$R/share/applications" "${DEBIAN_APPS:-$HOME/rvwasm/debian-root-release/usr/share/applications}:$SYS/share/applications" $NATIVE_APPS
# The native perl (docs/native-perl.md of branch nativeperl): a static wasm perl 5.40.1 with Debian's XS modules built in, which
# tools/debian-enter binds over /usr/bin/perl (and /run/native/perl is where its @INC says it is) when this disk is mounted.
NATIVEPERL=${NATIVEPERL:-$HOME/rvwasm/nativeperl/pack/usr/lib/nativeperl}
if [ -x "$NATIVEPERL/bin/perl" ]; then
	mkdir -p "$R/perl"
	cp -a "$NATIVEPERL/." "$R/perl/"
fi
{
	echo "nqs disk: shared Qt 6.8.2 / KF6 6.13 / ICU 76.1 closure of KCalc 24.12.3 and Konsole 24.12.3, ld-wasm."
	echo "repo: ${NQS_REV:-$(git -C "$REPO" rev-parse --short HEAD 2> /dev/null || echo unknown)}"
	echo "sysroot-shared: $SYS"
	[ -x "$R/perl/bin/perl" ] && echo "perl: $(cd "$R/perl" && find . -type f | sort | xargs cksum | cksum | cut -d' ' -f1) ($NATIVEPERL)"
	true
} > "$R/share/nqs/info.txt"

# Files Qt maps MAP_SHARED (plugins, the MIME cache) that nqs-run copies to memory: "size path-below-usr".
(cd "$R/usr" && { find lib/qt6/plugins share/mime/mime.cache -type f; [ -d lib/qt6/qml ] && find lib/qt6/qml -type f -name '*.so'; true; } | sort | while read -r f; do echo "$(stat -c %s "$f") $f"; done) \
	> "$R/share/nqs/maps.list"

# Reproducible: owners, modes, times.
find "$R" -type d -exec chmod 755 {} +
find "$R" -type f ! -perm /111 -exec chmod 644 {} +
find "$R" -exec touch -h -d "@$EPOCH" {} +

# The manifest of files (path, size) that goes into the image too.
(cd "$R" && find . -mindepth 1 \( -type f -o -type l \) | sort | while read -r f; do
	if [ -L "$f" ]; then echo "l ${f#./} -> $(readlink "$f")"; else echo "f ${f#./} $(stat -c %s "$f")"; fi
done) > "$OUT/files.txt"
cp "$OUT/files.txt" "$R/share/nqs/files.txt"
touch -d "@$EPOCH" "$R/share/nqs/files.txt" "$R/share/nqs" "$R/share"

# The image: ext4, no journal (read-only), no reserved blocks, everything written now, sized to the content.
payload=$(du -sb "$R" | cut -f1)
inodes=$(( $(find "$R" | wc -l) + 64 ))
mib=$(( payload / 1048576 * 103 / 100 + 3 ))
IMG=$OUT/nqs.ext4
while :; do
	rm -f "$IMG"
	if fakeroot -- sh -c "chown -R 0:0 '$R' && mke2fs -q -t ext4 -b 4096 -L nqs -m 0 -N $inodes -O ^has_journal,^resize_inode \
		-U 6e717321-0000-4000-8000-00000000d102 -E hash_seed=6e717321-0000-4000-8000-00000000d102,lazy_itable_init=0,root_owner=0:0 \
		-d '$R' '$IMG' ${mib}M" 2> "$OUT/mke2fs.log"; then
		break
	fi
	mib=$((mib + 1))
	[ $mib -lt 400 ] || { cat "$OUT/mke2fs.log"; exit 1; }
done
# mke2fs -d copies the files' access and change times (the time of this build): set them all to the fixed time.
{
	echo "sif / atime $EPOCH"; echo "sif / ctime $EPOCH"; echo "sif / mtime $EPOCH"; echo "sif / crtime $EPOCH"
	(cd "$R" && find . -mindepth 1 | sort | sed 's|^\.||') | while read -r f; do
		for t in atime ctime mtime crtime; do echo "sif \"$f\" $t $EPOCH"; done
	done
} > "$OUT/times.dbg"
debugfs -w -f "$OUT/times.dbg" "$IMG" > /dev/null 2>&1
e2fsck -fn "$IMG" | tail -2
python3 "$REPO/tools/split-disk.py" "$IMG" "$PIECES"
echo "nqs.ext4: $(stat -c %s "$IMG") bytes ($mib MiB), $(wc -l < "$OUT/files.txt") files, payload $payload bytes"

# The library manifest (tools/dl-manifest.py): every wasm library and plugin in usr/lib; the libraries KCalc needs are
# preloaded (compiled at page load of a native launch), the rest (Konsole's own, plugins) are listed, not preloaded.
KC=$(for m in $PRELOAD_PROGS; do
	(cd "$R/usr" && python3 "$REPO/nativeqt/shared/needed-closure.py" $m "$R/usr/lib" | head -n -1 | awk '{print $2}')
done | sort -u)
# PRELOAD_SELF: the modules themselves too (plugins a thread dlopens would be compiled by every thread, not taken from the page)
# and what they need (a QML plugin's libraries: Kirigami, QuickTemplates2, ... are dlopened with it, after the threads exist)
if [ -n "${PRELOAD_SELF:-}" ]; then
	NEEDS=$(for m in $PRELOAD_SELF; do
		(cd "$R/usr" && python3 "$REPO/nativeqt/shared/needed-closure.py" $m "$R/usr/lib" 2> /dev/null | head -n -1 | awk '{print $2}')
	done | sort -u)
	KC=$( { echo "$KC"; echo "$NEEDS"; for m in $PRELOAD_SELF; do basename "$m"; done; } | sort -u)
fi
PRE=()
for n in $KC; do PRE+=(--preload "$n"); done
python3 "$REPO/nativeqt/shared/dl-order.py" "$R/usr" "$OUT/dl-order.txt"
rm -rf "$OUT/dl"
python3 "$REPO/tools/dl-manifest.py" --out "$OUT/dl" --order "$OUT/dl-order.txt" "${PRE[@]}" $(find "$R/usr/lib" -type f -name '*.so*' | sort) > "$OUT/dl.log"
echo "dl: $(grep -c . "$OUT/dl.log") libraries, $(grep -vc 'not preloaded' "$OUT/dl.log") preloaded; $(du -sh "$OUT/dl" | cut -f1)"
