#!/bin/bash
# A piece of initramfs for trying the sound card under tools/node-machine.js (tests/audio/run.sh), made in the
# build VM: Debian's aplay, speaker-test and amixer (alsa-utils, riscv64) with libasound and alsa-lib's
# configuration, test sounds in /audio, this tree's rv (wasm) as /bin/rv, its /init (which makes /dev/snd) and
# its debian-enter as /bin/debian (which gives Debian /dev/snd).
# Nothing of the machine's disks is touched: the libraries are copied out of ~/rvwasm/debian-root-kde.
#   [RVW=DIR] tests/audio/overlay.sh [TREE]   TREE: this source tree as the VM has it (default ~/wt/audio);
#                                             RVW: where its rv for wasm was built
# Writes ~/rvwasm/audio-test/overlay.cpio.gz.
set -e
S=${1:-$HOME/wt/audio}
W=~/rvwasm/audio-test
D=~/rvwasm/debian-root-kde
RVW=${RVW:-$HOME/rvwasm/build-rv-wasm-audio}
mkdir -p $W/debs
cd $W
[ -s Packages ] || curl -s http://deb.debian.org/debian/dists/trixie/main/binary-riscv64/Packages.xz | xz -d > Packages
deb=$(grep -A20 "^Package: alsa-utils$" Packages | sed -n 's/^Filename: //p' | head -1)
[ -s debs/$(basename $deb) ] || curl -s -o debs/$(basename $deb) http://deb.debian.org/debian/$deb
rm -rf x root && mkdir -p x root/usr/bin root/lib root/usr/share root/audio root/bin
dpkg-deb -x debs/$(basename $deb) x
cp x/usr/bin/aplay x/usr/bin/speaker-test x/usr/bin/amixer root/usr/bin/
ln -s aplay root/usr/bin/arecord
cp -r x/usr/share/sounds root/usr/share/ 2>/dev/null || true
cp -r $D/usr/share/alsa root/usr/share/alsa
# The libraries they name, and what those name in turn, unless the machine's /lib has them (the stage puts glibc
# there: tools/stage.sh).
have=$(zcat ~/rvwasm/www/initramfs-wasm64_nommu.cpio.gz | cpio -t 2>/dev/null | sed -n 's|^lib/||p')
todo=(root/usr/bin/aplay root/usr/bin/speaker-test root/usr/bin/amixer)
while [ ${#todo[@]} -gt 0 ]; do
	next=()
	for f in "${todo[@]}"; do
		for lib in $(riscv64-linux-gnu-readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
			echo "$have" | grep -qx "$lib" && continue
			[ -e root/lib/$lib ] && continue
			src=$(ls $D/usr/lib/riscv64-linux-gnu/$lib $D/lib/riscv64-linux-gnu/$lib 2>/dev/null | head -1)
			[ -n "$src" ] || { echo "missing $lib (for $f)"; exit 1; }
			cp -L "$src" root/lib/$lib
			next+=(root/lib/$lib)
		done
	done
	todo=("${next[@]}")
done
# Test sounds: a sine of 440 Hz, 2 s, at 48000 Hz stereo S16; and 1 kHz at 44100 Hz mono S16.
python3 - <<'EOF'
import math, struct, wave
def write(path, rate, channels, freq, seconds):
    w = wave.open(path, "wb")
    w.setnchannels(channels); w.setsampwidth(2); w.setframerate(rate)
    frames = bytearray()
    for k in range(int(rate * seconds)):
        v = int(12000 * math.sin(2 * math.pi * freq * k / rate))
        frames += struct.pack("<" + "h" * channels, *([v] * channels))
    w.writeframes(bytes(frames))
    w.close()
write("root/audio/a440-48k-stereo.wav", 48000, 2, 440, 2)
write("root/audio/a1000-44k-mono.wav", 44100, 1, 1000, 2)
EOF
cp $RVW/rv root/bin/rv
cp $S/tools/initramfs-init root/init
cp $S/tools/debian-enter root/bin/debian
chmod 755 root/bin/rv root/bin/debian root/init root/usr/bin/*
(cd root && find . -mindepth 1 | sort | cpio -o --format newc -R 0:0 2>/dev/null | gzip -1 > $W/overlay.cpio.gz)
ls -la $W/overlay.cpio.gz
du -sh root/lib
