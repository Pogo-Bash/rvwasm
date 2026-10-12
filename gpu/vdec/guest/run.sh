# Run a player on the page's window server with the page's video decoder, and wait for it:
#   bash /tmp/v/run.sh NAME TIMEOUT PROGRAM [ARGS...]        (in the guest, inside `debian`)
# The bundle (/tmp/v: mpv, ffmpeg, vainfo and the libraries they need that the disk lacks, and the driver) is
# gpu/tools/vdec-bundle.sh's. The log is /root/v-NAME.log; what says the page decoded is shown after.
name=$1 t=$2; shift 2
V=$(cd "$(dirname "$0")" && pwd)
export RVGPU=1 RVGPU_DRM=1 RVGPU_STATS=1 WLSERVER_STATS=1 PATH="$V/bin:$PATH:/run/rvgpu/bin"
export LD_LIBRARY_PATH="$V/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# (the driver in the initramfs, /run/rvgpu/va, unless the bundle's is newer or the initramfs has none)
[ -e /run/rvgpu/va/rvwasm_drv_video.so ] && [ -z "$BUNDLE_DRIVER" ] || export LIBVA_DRIVERS_PATH=$V/va
SECONDS=0
timeout "$t" desktop wayland stdbuf -oL -eL "$@" > /root/v-$name.log 2>&1 < /dev/null
echo "== $name: status $? in ${SECONDS} s: $*"
grep -a -E "hwdec|[Hh]ardware decoding|rvvdec|VO: |\(vaapi\)|[Ee]rror|failed|Dropped|Exiting" /root/v-$name.log | grep -a -v "^libva info" | tail -${TAILN:-14}
grep -a -E "video (client|decoder|decoding)" /root/rvgpu.log | tail -3
