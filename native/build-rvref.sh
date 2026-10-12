#!/bin/bash
# The riscv64 reference of a test program: the same source built against Debian trixie's own Qt
# (headers from Debian's -dev packages, libraries of the Debian root), to run under rv for comparison.
#   build-rvref.sh NAME   -> $NQ/out/NAME-test.rv  (to be run inside the Debian root)
set -e
. "$(dirname "$0")/env.sh"
n=$1
R=$HOME/rvwasm/debian-root-kde
H=$NQ/rvref
mkdir -p $H/deb
pool=http://deb.debian.org/debian/pool/main
for d in q/qt6-base/qt6-base-dev_6.8.2+dfsg-9+deb13u2_riscv64.deb q/qt6-declarative/qt6-declarative-dev_6.8.2+dfsg-7_riscv64.deb; do
	f=$H/deb/$(basename $d)
	[ -e $f ] || curl -sfL -o $f $pool/$d
	[ -e $f.x ] || { dpkg-deb -x $f $H/root && touch $f.x; }
done
I=$H/root/usr/include/riscv64-linux-gnu/qt6
T=$NQ_TREE/tests/$n
mods="Widgets Gui Core"
extra=()
if [ $n = qquick ]; then
	mods="Quick Qml QmlModels OpenGL Network Gui Core"
	# main.qml into the program, as qt_add_resources does (the host's rcc).
	(cd $T && $NQ/host/libexec/rcc --name qml -o $NQ_BUILD/rvref-qrc-$n.cpp <(printf '<RCC><qresource prefix="/"><file>main.qml</file></qresource></RCC>\n') 2>/dev/null) ||
		(cd $T && printf '<RCC><qresource prefix="/"><file>main.qml</file></qresource></RCC>\n' > $NQ_BUILD/rvref-$n.qrc && cp main.qml $NQ_BUILD/ && $NQ/host/libexec/rcc --name qml -o $NQ_BUILD/rvref-qrc-$n.cpp $NQ_BUILD/rvref-$n.qrc)
	extra+=($NQ_BUILD/rvref-qrc-$n.cpp)
fi
incs=(-I$I)
libs=()
for m in $mods; do incs+=(-I$I/Qt$m); libs+=($R/usr/lib/riscv64-linux-gnu/libQt6$m.so.6); done
mkdir -p $NQ/out
riscv64-linux-gnu-g++ -O2 -std=c++17 -fPIC --sysroot=$R "${incs[@]}" -o $NQ/out/$n-test.rv $T/main.cpp "${extra[@]}" \
	-Wl,-rpath-link,$R/usr/lib/riscv64-linux-gnu "${libs[@]}"
ls -la $NQ/out/$n-test.rv
