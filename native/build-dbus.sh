#!/bin/bash
# libdbus-1 (D-Bus 1.16.2, Debian trixie's) as a static library for QtDBus: the library only (the bus
# daemon a session uses is Debian's riscv64 one under rv), into the sysroot.
set -e
. "$(dirname "$0")/env.sh"
. $NQ_TREE/meson-cross.sh
export NQ_LAUNCHER=ccache
[ -d $NQ_SRC/dbus ] || $NQ_TREE/fetch-kde.sh dbus
B=$NQ_BUILD/dbus
meson setup --wipe $B $NQ_SRC/dbus --cross-file $NQ_CROSS --prefix=/usr --libdir=lib -Ddefault_library=static \
	-Dmessage_bus=false -Dtools=false -Dmodular_tests=disabled -Dintrusive_tests=false -Dvalgrind=disabled \
	-Ddoxygen_docs=disabled -Dxml_docs=disabled -Dducktype_docs=disabled -Dqt_help=disabled \
	-Dsystemd=disabled -Dselinux=disabled -Dapparmor=disabled -Dlibaudit=disabled \
	-Dlaunchd=disabled -Dkqueue=disabled -Dinotify=disabled -Dasserts=false -Dchecks=false \
	-Dverbose_mode=false -Dstats=false -Duser_session=false \
	-Dsession_socket_dir=/tmp -Dsystem_socket=/run/dbus/system_bus_socket
ninja -j$NQ_JOBS $NQ_NINJA_FLAGS -C $B
DESTDIR=$NQ_SYSROOT ninja -C $B install > /dev/null
# Its pkg-config Cflags say -pthread, which Qt's CMake takes for a macro to define (-D-pthread).
sed -i 's/ -pthread//' $NQ_SYSROOT/usr/lib/pkgconfig/dbus-1.pc
echo dbus ok
