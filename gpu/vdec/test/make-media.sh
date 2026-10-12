#!/bin/bash
# Test media for the video decoder, made in the build VM with Debian's riscv64 ffmpeg under qemu (x264, x265,
# libvpx, SVT-AV1, libaom): synthetic pictures (testsrc2), so nothing is downloaded.
#   gpu/vdec/test/make-media.sh [small|owner|all]      -> ~/vdec/media
# small: 640x360, 2 s, one file per codec and per H.264 feature the driver has to get right (B-frames,
# scaling lists, weighted prediction, slices, CAVLC, Main/Baseline, 4:2:2 that must be refused, interlaced
# that must be refused); hidden frames for VP8 and VP9 (alt-ref). the clips the manual video check plays
# (H.264 1080p, VP9 4K, AV1 4K, HEVC 1080p, VP8 720p).
set -e
W=${W:-$HOME/vdec}
R=$HOME/rvwasm/debian-root-kde
X=$W/x
M=$W/media
mkdir -p $M
LDP=$X/usr/lib/riscv64-linux-gnu:$R/usr/lib/riscv64-linux-gnu/blas:$R/usr/lib/riscv64-linux-gnu/lapack
ff() { qemu-riscv64-static -L $R -E LD_LIBRARY_PATH=$LDP $X/usr/bin/ffmpeg -y -nostdin -v error "$@" < /dev/null; }
src() { echo "testsrc2=size=$1:rate=${3:-30}:duration=$2"; }
make() {
	local out=$1; shift
	[ -s $M/$out ] && return
	echo "making $out"
	ff -f lavfi -i "$1" -threads 4 "${@:2}" $M/$out.part.${out##*.} && mv $M/$out.part.${out##*.} $M/$out
}
# Two passes (libvpx makes alt-ref frames, which are hidden, only then)
make2() {
	local out=$1; shift
	[ -s $M/$out ] && return
	echo "making $out (2 passes)"
	ff -f lavfi -i "$1" -threads 4 "${@:2}" -pass 1 -passlogfile $M/$out.pass -f null - &&
	ff -f lavfi -i "$1" -threads 4 "${@:2}" -pass 2 -passlogfile $M/$out.pass $M/$out.part.${out##*.} &&
	mv $M/$out.part.${out##*.} $M/$out; rm -f $M/$out.pass*
}
what=${1:-small}
if [ $what = small ] || [ $what = all ]; then
	S=$(src 640x360 2)
	make h264-high.mp4 "$S" -c:v libx264 -preset fast -profile:v high -bf 3
	make h264-cqm.mp4 "$S" -c:v libx264 -preset fast -x264-params cqm=jvt:weightp=2:bframes=3:b-pyramid=normal
	make h264-slices.mp4 "$S" -c:v libx264 -preset fast -x264-params slices=4:ref=5
	make h264-cavlc.mp4 "$S" -c:v libx264 -preset fast -profile:v main -x264-params cabac=0:weightb=1
	make h264-baseline.mp4 "$S" -c:v libx264 -preset fast -profile:v baseline
	make h264-intra.mp4 "$S" -c:v libx264 -preset fast -x264-params keyint=1
	make h264-422.mp4 "$S" -c:v libx264 -preset fast -pix_fmt yuv422p
	make h264-interlaced.mp4 "$S" -c:v libx264 -preset fast -flags +ildct+ilme -x264-params tff=1
	make2 vp9-altref.webm "$S" -c:v libvpx-vp9 -deadline good -cpu-used 8 -lag-in-frames 16 -auto-alt-ref 1 -b:v 500k
	make2 vp8-altref.webm "$S" -c:v libvpx -deadline good -cpu-used 8 -lag-in-frames 16 -auto-alt-ref 1 -b:v 500k
	make hevc-main.mp4 "$S" -c:v libx265 -preset fast -x265-params bframes=4:log-level=error
	make hevc-wpp.mp4 "$S" -c:v libx265 -preset fast -x265-params bframes=3:wpp=1:slices=2:scaling-list=default:weightb=1:log-level=error
	make av1.mkv "$S" -c:v libsvtav1 -preset 10
	make av1-aom.mkv "$S" -c:v libaom-av1 -cpu-used 8 -lag-in-frames 16 -b:v 300k
fi
if [ $what = owner ] || [ $what = all ]; then
	make h264-1080p.mp4 "$(src 1920x1080 20)" -c:v libx264 -preset veryfast -profile:v high -bf 3 -b:v 6M
	make hevc-1080p.mp4 "$(src 1920x1080 10)" -c:v libx265 -preset ultrafast -x265-params bframes=3:log-level=error -b:v 4M
	make2 vp8-720p.webm "$(src 1280x720 10)" -c:v libvpx -deadline good -cpu-used 8 -lag-in-frames 16 -auto-alt-ref 1 -b:v 2M
	make2 vp9-4k.webm "$(src 3840x2160 10)" -c:v libvpx-vp9 -deadline good -cpu-used 8 -row-mt 1 -lag-in-frames 16 -auto-alt-ref 1 -b:v 12M
	make av1-4k.mkv "$(src 3840x2160 10)" -c:v libsvtav1 -preset 12 -b:v 10M
fi
ls -la $M
