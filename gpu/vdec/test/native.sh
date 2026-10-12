#!/bin/bash
# Native test of rvwasm_drv_video.so in the build VM: Debian's riscv64 ffmpeg under rv (native build, its
# emulated render node) decodes with -hwaccel vaapi through the driver, which talks to vdec-native-server.py
# (gpuserver and the page in one, libavcodec standing in for WebCodecs); the frames it copies back
# (vaGetImage) are compared with the same file decoded in software (qemu-riscv64), frame by frame (framemd5).
#   gpu/vdec/test/native.sh FILE [FRAMES] [EXTRA_FFMPEG_ARGS...]      (in the VM)
#   gpu/vdec/test/native.sh export     export-test.c: zero copy's export and import across opens
# Environment: GPU=1 says zero copy is possible (the driver then also exports, if asked); KEEP=1 keeps the
# logs in /tmp/vdec-native; RV= another rv; DEBUG=1 the driver's messages.
set -e
file=$1; frames=${2:-30}; shift 2 || shift $#
T=$(cd "$(dirname "$0")" && pwd)
W=${W:-$HOME/vdec}
R=$HOME/rvwasm/debian-root-kde
X=$W/x
RV=${RV:-$W/build-rv/rv}
O=/tmp/vdec-native; mkdir -p $O
LDP=$X/usr/lib/riscv64-linux-gnu:$R/usr/lib/riscv64-linux-gnu/blas:$R/usr/lib/riscv64-linux-gnu/lapack
Q="qemu-riscv64-static -L $R -E LD_LIBRARY_PATH=$LDP"
# the stand-in decoder
if [ ! -e $W/out/vdec-ref ] || [ $T/vdec-ref.c -nt $W/out/vdec-ref ]; then
	riscv64-linux-gnu-gcc -O2 -I$W/devx/usr/include/riscv64-linux-gnu -o $W/out/vdec-ref $T/vdec-ref.c -Wl,--allow-shlib-undefined \
		$R/usr/lib/riscv64-linux-gnu/libavcodec.so.61 $R/usr/lib/riscv64-linux-gnu/libavutil.so.59
fi
S=$O/server.sock
pkill -f "vdec-native-server.py $S" 2>/dev/null || true
reorder=0
[ -e "$file" ] && reorder=$($Q $X/usr/bin/ffprobe -v error -select_streams v:0 -show_entries stream=has_b_frames -of csv=p=0 "$file" < /dev/null)
python3 $T/vdec-native-server.py $S ${GPU:+--gpu} --reorder ${reorder:-0} --ref "$Q $W/out/vdec-ref" --log $O/server.log &
spid=$!
for i in $(seq 50); do [ -S $S ] && break; sleep 0.1; done
if [ "$file" = export ]; then
	riscv64-linux-gnu-gcc -O2 -I$X/usr/include -o $W/out/export-test $T/export-test.c -Wl,--allow-shlib-undefined \
		$R/usr/lib/riscv64-linux-gnu/libva.so.2 $R/usr/lib/riscv64-linux-gnu/libva-drm.so.2
	timeout 120 /usr/bin/env -i PATH=$R/usr/bin RV_SYSROOT=$R HOME=/tmp RV_VIRTGPU=$S RV_VIRTGPU_IMPORT=1 \
		RVVDEC_SOCKET=$S LIBVA_DRIVER_NAME=rvwasm LIBVA_DRIVERS_PATH=$W/out ${DEBUG:+RVVDEC_DEBUG=1 RV_VIRTGPU_DEBUG=1} \
		$RV $W/out/export-test; rc=$?
	kill $spid 2>/dev/null; wait $spid 2>/dev/null
	tail -3 $O/server.log
	exit $rc
fi
base=$(basename "$file")
rm -f $O/$base.chunks
set +e
# software reference (qemu: much faster than rv's interpreter for a whole decode)
$Q $X/usr/bin/ffmpeg -y -nostdin -v error -i "$file" -frames:v $frames -an -pix_fmt yuv420p -f framemd5 $O/$base.sw.md5 < /dev/null
SECONDS=0
timeout 900 /usr/bin/env -i PATH=$R/usr/bin RV_SYSROOT=$R HOME=/tmp RV_VIRTGPU=$S RVVDEC_SOCKET=$S \
	LIBVA_DRIVER_NAME=rvwasm LIBVA_DRIVERS_PATH=$W/out LD_LIBRARY_PATH=$LDP ${DEBUG:+RVVDEC_DEBUG=1} \
	RVVDEC_DUMP=$O/$base.chunks $RV $X/usr/bin/ffmpeg -y -nostdin -v ${FFV:-warning} -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
	-i "$file" -frames:v $frames -an "$@" -pix_fmt yuv420p -f framemd5 $O/$base.hw.md5 2> $O/$base.hw.log < /dev/null
rc=$?
kill $spid 2>/dev/null; wait $spid 2>/dev/null
echo "== $base: rv ffmpeg rc $rc in ${SECONDS} s"
tail -5 $O/$base.hw.log
grep -v "^#" $O/$base.sw.md5 | awk -F, '{print $NF}' > $O/sw.list
grep -v "^#" $O/$base.hw.md5 2>/dev/null | awk -F, '{print $NF}' > $O/hw.list
n_sw=$(wc -l < $O/sw.list); n_hw=$(wc -l < $O/hw.list); same=$(paste -d' ' $O/sw.list $O/hw.list | awk '$1==$2' | wc -l)
echo "frames: software $n_sw, through the driver $n_hw, the same $same"
[ -s $O/$base.chunks ] && python3 $T/chunks.py $O/$base.chunks || echo "chunks: none (ffmpeg did not use the driver)"
tail -3 $O/server.log
if [ -n "$EXPECT_FALLBACK" ]; then
	[ ! -s $O/$base.chunks ] || [ "$n_hw" -lt "$n_sw" ] && echo "PASS $base (refused, as it should be)" || echo "FAIL $base (not refused)"
else
	[ "$n_sw" -gt 0 ] && [ "$same" = "$n_sw" ] && [ -s $O/$base.chunks ] && echo "PASS $base" || echo "FAIL $base"
fi
