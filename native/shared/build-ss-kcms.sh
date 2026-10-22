#!/bin/bash
# The System Settings modules (ss-kcms/CMakeLists.txt) as shared plugins into sysroot-shared.
#   shared/build-ss-kcms.sh      (after build-kf-shared.sh plasma-workspace-src: the pristine trees in $NQ/src-shared)
set -e
. "$(dirname "$0")/env.sh"
export NQ_LAUNCHER=ccache
opt=${NQ_KDE_OPT:--O2}
B=$NQ_BUILD/ss-kcms
ht=$(f=$(find $NQ/host -name KF6ConfigCompilerTargets.cmake 2>/dev/null | head -1); [ -n "$f" ] && dirname "$(dirname "$f")" || echo $NQ/host/lib/cmake)
for n in plasma-workspace plasma-desktop kde-cli-tools; do
	S=$NQ/src-shared/$n
	if [ ! -d $S ]; then
		mkdir -p $S && tar xf $(ls $NQ/src/dl/$n-[0-9]*.tar.xz $NQ/src-shared/dl/$n-[0-9]*.tar.xz 2> /dev/null | head -1) -C $S --strip-components=1
		rm -rf $S/po $S/poqm
		[ -f $NQ_TREE/patches/nqshared-$n.diff ] && patch -d $S -p1 -s -N < $NQ_TREE/patches/nqshared-$n.diff
	fi
done
cmake -S $NQS_TREE/ss-kcms -B $B -G Ninja -DPW_SRC=$NQ/src-shared/plasma-workspace -DPD_SRC=$NQ/src-shared/plasma-desktop -DCLI_SRC=$NQ/src-shared/kde-cli-tools \
	-DBUILD_TESTING=OFF -DKDE_INSTALL_USE_QT_SYS_PATHS=ON -DKDE_INSTALL_QTPLUGINDIR=lib/qt6/plugins -DKDE_INSTALL_PLUGINDIR=lib/qt6/plugins -DKDE_INSTALL_QMLDIR=lib/qt6/qml \
	-DKDE_SKIP_TEST_SETTINGS=ON -DBUILD_WITH_QT6=ON -DQT_MAJOR_VERSION=6 \
	-DCMAKE_TOOLCHAIN_FILE=$NQ_SYSROOT/usr/lib/cmake/Qt6/qt.toolchain.cmake -DQT_HOST_PATH=$NQ/host \
	-DKF6_HOST_TOOLING=$ht -DKF_IGNORE_PLATFORM_CHECK=ON \
	-DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_STAGING_PREFIX=$NQ_SYSROOT/usr -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=None \
	-DCMAKE_SKIP_RPATH=ON -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
	-DCMAKE_C_FLAGS="$opt -DNDEBUG" -DCMAKE_CXX_FLAGS="$opt -DNDEBUG -DQT_NO_DEBUG" $NQS_KCM_EXTRA
ninja -j$NQ_JOBS -k ${NQS_NINJA_K:-1} -C $B
ninja -C $B install > $B/install.log
echo "ss-kcms done"
