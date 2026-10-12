#!/bin/bash
# The guest's side of the video decoder checks, in the build VM: the driver (gpu/vdec/driver), and Debian
# trixie's riscv64 mpv, ffmpeg and vainfo with the libraries they need that the disks lack (the closure against
# debian-root-kde: libva-dev for the driver's headers, mpv's libmujs3, libsixel1, ... in ~/vdec/x), as v.tgz;
# the test media (gpu/vdec/test/make-media.sh) beside it.
#   gpu/tools/vdec-bundle.sh [TREE] [OUT]       (in the VM; TREE: this tree there, OUT: ~/vdec/www)
set -e
S=${1:-$HOME/wt/vdec}
OUT=${2:-$HOME/vdec/www}
W=$HOME/vdec
X=$W/x
R=$HOME/rvwasm/debian-root-kde
# the packages, once (their names: what resolve.py found missing from debian-root-kde for mpv, ffmpeg, vainfo,
# libva-dev; pool paths of trixie's riscv64 archive)
if [ ! -x $X/usr/bin/mpv ] || [ ! -e $X/usr/include/va/va_backend.h ]; then
	mkdir -p $W/debs
	for f in pool/main/m/mpv/mpv_0.40.0-3+deb13u1_riscv64.deb pool/main/f/ffmpeg/ffmpeg_7.1.5-0+deb13u1_riscv64.deb \
		pool/main/f/ffmpeg/libavdevice61_7.1.5-0+deb13u1_riscv64.deb pool/main/libv/libva-utils/vainfo_2.22.0+ds1-2_riscv64.deb \
		pool/main/libv/libva/libva-dev_2.22.0-3_riscv64.deb pool/main/libv/libva/libva-glx2_2.22.0-3_riscv64.deb \
		pool/main/m/mujs/libmujs3_1.3.6-1_riscv64.deb pool/main/libs/libsixel/libsixel1_1.10.5-1_riscv64.deb \
		pool/main/j/jackd2/libjack-jackd2-0_1.9.22~dfsg-4_riscv64.deb pool/main/o/openal-soft/libopenal1_1.24.2-1_riscv64.deb \
		pool/main/s/sndio/libsndio7.0_1.10.0-0.1_riscv64.deb pool/main/libx/libxpresent/libxpresent1_1.0.1-1+b2_riscv64.deb \
		pool/main/libc/libcdio/libcdio19t64_2.2.0-4.1~deb13u1_riscv64.deb \
		pool/main/libc/libcdio-paranoia/libcdio-cdda2t64_10.2+2.0.2-1+b1_riscv64.deb \
		pool/main/libc/libcdio-paranoia/libcdio-paranoia2t64_10.2+2.0.2-1+b1_riscv64.deb; do
		[ -e $W/debs/$(basename $f) ] || curl -sfL -o $W/debs/$(basename $f) http://deb.debian.org/debian/$f
		dpkg-deb -x $W/debs/$(basename $f) $X
	done
fi
mkdir -p $W/out $OUT
VA_INC=$X/usr/include $S/gpu/vdec/driver/build.sh $W/out > /dev/null
rm -rf $W/bundle && mkdir -p $W/bundle/v/bin $W/bundle/v/lib $W/bundle/v/va
cp $X/usr/bin/mpv $X/usr/bin/ffmpeg $X/usr/bin/ffprobe $X/usr/bin/vainfo $W/bundle/v/bin/
for f in $X/usr/lib/riscv64-linux-gnu/*.so.*; do
	case $(basename $f) in libdrm*|libpciaccess*) continue ;; esac
	cp -a $f $W/bundle/v/lib/
done
cp $W/out/rvwasm_drv_video.so $W/bundle/v/va/
cp $S/gpu/vdec/guest/run.sh $W/bundle/v/run.sh
tar -C $W/bundle -czf $OUT/v.tgz v
mkdir -p $OUT/media
for f in $W/media/*; do
	case $f in *.part.*|*.pass*) ;; *) ln -f $f $OUT/media/ 2>/dev/null || cp $f $OUT/media/ ;; esac
done
ls -la $OUT/v.tgz; ls $OUT/media
