#!/bin/bash
# KDE Frameworks / Plasma pieces for native linux-wasm programs (static), in the order given; a finished one
# is skipped (NQ_REDO=1: again, incrementally). Options as Debian's packaging (dh kf6: BUILD_TESTING off,
# KDE_INSTALL_USE_QT_SYS_PATHS) plus static, no docs, no Python bindings, no translations' catalogs.
#   build-kf.sh [-host] NAME...    (-host: for the build VM itself, with the host Qt, for the KF6 tools a
#                                   cross build runs: kconfig_compiler and the like; into $NQ/host)
# Optimisation: NQ_KDE_OPT (default -O1 for development builds; -O2 for the ones we measure).
set -e
. "$(dirname "$0")/env.sh"
export NQ_LAUNCHER=ccache
host=0
[ "$1" = -host ] && { host=1; shift; }
opt=${NQ_KDE_OPT:--O1}
for n in "$@"; do
	[ -d $NQ_SRC/$n ] || $NQ_TREE/fetch-kde.sh $n
	if [ $host = 1 ]; then B=$NQ_BUILD/host-kf-$n; else B=$NQ_BUILD/kf-$n; fi
	[ -e $B/.nq-installed ] && [ -z "$NQ_REDO" ] && continue
	t0=$(date +%s)
	# No translations for now: their catalogs need Qt's lrelease (qttools), which the host Qt has not got.
	[ -z "$NQ_TRANSLATIONS" ] && rm -rf $NQ_SRC/$n/po $NQ_SRC/$n/poqm
	# Ours (patches/nativeqt-NAME.diff), once.
	if [ -f $NQ_TREE/patches/nativeqt-$n.diff ] && [ ! -e $NQ_SRC/$n/.nq-patched ]; then
		patch -d $NQ_SRC/$n -p1 -s -N < $NQ_TREE/patches/nativeqt-$n.diff
		touch $NQ_SRC/$n/.nq-patched
	fi
	# Where the host's KF6 tools' CMake files are (kconfig_compiler: build-kf.sh -host kconfig).
	ht=$(f=$(find $NQ/host -name KF6ConfigCompilerTargets.cmake 2>/dev/null | head -1); [ -n "$f" ] && dirname "$(dirname "$f")" || echo $NQ/host/lib/cmake)
	common=(-G Ninja $NQ_KF_EXTRA -DBUILD_TESTING=OFF -DBUILD_QCH=OFF -DBUILD_DOC=OFF -DBUILD_PYTHON_BINDINGS=OFF
		-DKDE_INSTALL_USE_QT_SYS_PATHS=ON -DKDE_SKIP_TEST_SETTINGS=ON -DBUILD_WITH_QT6=ON -DQT_MAJOR_VERSION=6)
	if [ $host = 1 ]; then
		cmake -S $NQ_SRC/$n -B $B "${common[@]}" -DCMAKE_PREFIX_PATH=$NQ/host -DCMAKE_INSTALL_PREFIX=$NQ/host \
			-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
	else
		cmake -S $NQ_SRC/$n -B $B "${common[@]}" \
			-DCMAKE_TOOLCHAIN_FILE=$NQ_SYSROOT/usr/lib/cmake/Qt6/qt.toolchain.cmake -DQT_HOST_PATH=$NQ/host \
			-DKF6_HOST_TOOLING=$ht -DKF_IGNORE_PLATFORM_CHECK=ON \
			-DCMAKE_INSTALL_PREFIX=$NQ_SYSROOT/usr -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=None \
			-DCMAKE_SKIP_RPATH=ON -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
			-DCMAKE_C_FLAGS="$opt -DNDEBUG" -DCMAKE_CXX_FLAGS="$opt -DNDEBUG -DQT_NO_DEBUG"
	fi
	ninja -j$NQ_JOBS $NQ_NINJA_FLAGS -C $B
	ninja -C $B install > /dev/null
	# Its CMake packages find their static dependencies (kf-static-deps.py): every package dir it installed.
	[ $host = 1 ] || python3 $NQ_TREE/kf-static-deps.py $NQ_SYSROOT $(sed -n 's|.*/usr/lib/cmake/\([^/]*\)/.*|\1|p' $B/install_manifest.txt | sort -u)
	touch $B/.nq-installed
	echo "kf $n$( [ $host = 1 ] && echo ' (host)'): $(( $(date +%s) - t0 )) s"
done
