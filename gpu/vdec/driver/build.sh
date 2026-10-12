#!/bin/bash
# Build rvwasm_drv_video.so (riscv64, for Debian trixie's libva 2.22) in the build VM:
#   gpu/vdec/driver/build.sh [OUT_DIR]      (in the VM; libva's headers from libva-dev, VA_INC)
# Default output: ~/vdec/out/rvwasm_drv_video.so. Warnings are errors.
set -e
cd "$(dirname "$0")"
OUT=${1:-$HOME/vdec/out}
VA_INC=${VA_INC:-$HOME/vdec/x/usr/include}
mkdir -p "$OUT"
riscv64-linux-gnu-gcc -O2 -g -fPIC -shared -fvisibility=hidden -Wall -Wextra -Werror -Wno-unused-parameter \
	-Wno-missing-field-initializers -I"$VA_INC" -o "$OUT/rvwasm_drv_video.so" \
	driver.c conn.c bits.c h264.c hevc.c vp8.c vp9.c av1.c -lpthread
riscv64-linux-gnu-nm -D --defined-only "$OUT/rvwasm_drv_video.so" | grep -q __vaDriverInit_1_
echo "built $OUT/rvwasm_drv_video.so"
