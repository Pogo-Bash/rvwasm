#!/bin/bash
# The programs of the GPU app matrix (VK-17) that the disks lack, in the build VM: Debian trixie's riscv64 `qml` (Qt Quick's runner; the
# disk has the Qt Quick libraries but not the tool), and GTK 4 (libgtk-4-1 4.18, its data and gtk4-widget-factory / gtk4-demo; the disk has
# GTK 3 and no GTK 4), as m.tgz. Their other libraries (pango, cairo, epoxy, ...) are on the disk: the closure against debian-root-release is
# these seven packages (~/vdec/resolve.py gives it: python3 ~/vdec/resolve.py ~/gpuwork/Packages ~/rvwasm/debian-root-release gtk-4-examples libgtk-4-bin qml-qt6).
#   gpu/tools/matrix-bundle.sh [OUT]        (in the VM; OUT: ~/vk17/www; tools/gpu-nm.sh fetches OUT/m.tgz into build/gpu-matrix-pkgs)
set -e
OUT=${1:-$HOME/vk17/www}
W=$HOME/vk17
X=$W/x
mkdir -p $W/debs $X $OUT
for f in pool/main/g/gtk4/gtk-4-examples_4.18.6+ds-2_riscv64.deb pool/main/g/graphene/libgraphene-1.0-0_1.10.8-5_riscv64.deb \
	pool/main/g/gtk4/libgtk-4-1_4.18.6+ds-2_riscv64.deb pool/main/c/cairo/libcairo-script-interpreter2_1.18.4-1+b1_riscv64.deb \
	pool/main/g/gtk4/libgtk-4-common_4.18.6+ds-2_all.deb pool/main/g/gtk4/libgtk-4-bin_4.18.6+ds-2_riscv64.deb \
	pool/main/q/qt6-declarative/qml-qt6_6.8.2+dfsg-7_riscv64.deb; do
	[ -e $W/debs/$(basename $f) ] || curl -sfL -o $W/debs/$(basename $f) http://deb.debian.org/debian/$f
	dpkg-deb -x $W/debs/$(basename $f) $X
done
rm -rf $W/bundle && mkdir -p $W/bundle/m/bin $W/bundle/m/lib $W/bundle/m/share/glib-2.0/schemas
cp $X/usr/lib/qt6/bin/qml $X/usr/bin/gtk4-widget-factory $X/usr/bin/gtk4-demo $W/bundle/m/bin/
cp -a $X/usr/lib/riscv64-linux-gnu/libgtk-4.so.1* $X/usr/lib/riscv64-linux-gnu/libgraphene-1.0.so.0* $X/usr/lib/riscv64-linux-gnu/libcairo-script-interpreter.so.2* $W/bundle/m/lib/
# (GTK's settings schemas, compiled: glib reads gschemas.compiled from every $XDG_DATA_DIRS/glib-2.0/schemas)
cp $X/usr/share/glib-2.0/schemas/*.xml $W/bundle/m/share/glib-2.0/schemas/
glib-compile-schemas $W/bundle/m/share/glib-2.0/schemas
rm -f $W/bundle/m/share/glib-2.0/schemas/*.xml
[ -d $X/usr/share/gtk-4.0 ] && cp -a $X/usr/share/gtk-4.0 $W/bundle/m/share/
# glmark2 (the riscv64 debs the GPU work fetched into ~/gpuwork/deb), for the page's machine, which has none
mkdir -p $W/gm $W/bundle/m/glmark2/share
for d in $HOME/gpuwork/deb/glmark2-es2-wayland_*_riscv64.deb $HOME/gpuwork/deb/glmark2-data_*_all.deb; do dpkg-deb -x $d $W/gm; done
cp $W/gm/usr/bin/glmark2-es2-wayland $W/bundle/m/glmark2/
cp -r $W/gm/usr/share/glmark2 $W/bundle/m/glmark2/share/
tar -C $W/bundle -czf $OUT/m.tgz m
ls -la $OUT/m.tgz; du -sh $W/bundle/m/*
