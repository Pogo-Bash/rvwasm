#!/usr/bin/env bash
# The public site's rate limits (docs/deploy.md "Abuse protection"), against the live server: a normal visit and a
# boot never get a 429, a spam loop does (with Retry-After), and the site answers again after the wait. Polite on
# purpose: about 100 MB of disk pieces, a few thousand small requests, over two minutes. Needs curl and python3.
#
#   tests/caddy-limits.sh [https://plasmaontheweb.com]
#
# It counts from this computer's address, so run it alone: the limits are per address, and a second run within a minute
# or two starts with part of the first one's requests still counted.
set -u
SITE=${1:-https://plasmaontheweb.com}
DISK=${RV_DISK:-release}
pass=0; fail=0
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 60 "$@"; }
# curl --parallel with a config file: every url needs its own output line, or the bodies go to the terminal
nullout() { awk '{print} /^url/ {print "output = \"/dev/null\""}' "$1" > "$1.n" && mv "$1.n" "$1"; }
tally() { sort | uniq -c | tr '\n' ' ' | sed 's/  */ /g'; }
check() { if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "  ok   $1"; else fail=$((fail+1)); echo "  FAIL $1 (wanted $3, got $2)"; fi; }

# the pieces the disk really has (the manifest's bitmap), so no request is a 404
curl -s "$SITE/disk/$DISK/manifest.json" -o /tmp/rv-manifest.json
python3 - <<'PY' > /tmp/rv-pieces.txt
import json
m = json.load(open("/tmp/rv-manifest.json"))
for i in range(m["size"] // m["piece"]):
    if (int(m["present"][i >> 2], 16) >> (3 - (i & 3))) & 1:
        print("%06d" % i)
PY
echo "disk pieces present: $(wc -l < /tmp/rv-pieces.txt)"

echo "1. a normal page load (landing page, boot page, its files, both big files revalidated, manifests, relay address)"
cat > /tmp/rv-load.txt <<URLS
url = "$SITE/"
url = "$SITE/assets/hero-960.webp"
url = "$SITE/favicon.ico"
url = "$SITE/site.webmanifest"
url = "$SITE/robots.txt"
url = "$SITE/desktop"
url = "$SITE/probe"
url = "$SITE/rv-site.json"
url = "$SITE/relay/address"
url = "$SITE/disk/$DISK/manifest.json"
url = "$SITE/disk/home/manifest.json"
URLS
for f in audio-worklet.js audio.js background.js breeze.css chrome.css disk-worker.js display.js gpu-exec.js probe.html \
         hostopen.css hostopen.js kernel-symbols.txt linux-worker.js linux.js panel.css plasma.js rvgpu-worker.js splash.js \
         vdec-worker.js vdec.js watchdog.js xterm.css xterm.js rvgpu/rvgpu.js vrend/vrend.js vrend/vrend.wasm; do
  echo "url = \"$SITE/$f\"" >> /tmp/rv-load.txt
done
# the big files by a 1-byte range (the count is what is tested, not 40 MB of download)
for f in vmlinux-wasm64_nommu.wasm initramfs-wasm64_nommu.cpio.gz rvgpu/rvgpu.wasm; do echo "url = \"$SITE/$f\"" >> /tmp/rv-load.txt; done
nullout /tmp/rv-load.txt
res=$(curl -s --parallel --parallel-max 6 --range 0-0 -o /dev/null -w '%{http_code}\n' --max-time 60 -K /tmp/rv-load.txt | tally)
echo "   $(grep -c '^url' /tmp/rv-load.txt) requests, 6 at a time: $res"
check "no 429 in a page load" "$(echo "$res" | grep -c 429)" 0
sleep 12

echo "2. a boot's disk: 100 pieces one after the other, as the disk worker fetches (full 1 MiB each)"
t0=$(date +%s)
res=$(head -100 /tmp/rv-pieces.txt | while read p; do code "$SITE/disk/$DISK/$p"; echo; done | tally)
echo "   $res in $(( $(date +%s) - t0 )) s"
check "no 429 in 100 sequential pieces" "$(echo "$res" | grep -c 429)" 0

echo "3. a fast boot: 8 pieces at a time, 400 requests (1-byte ranges, the count is what matters)"
head -400 /tmp/rv-pieces.txt | sed "s|.*|url = \"$SITE/disk/$DISK/&\"|" > /tmp/rv-par.txt
nullout /tmp/rv-par.txt
res=$(curl -s --parallel --parallel-max 8 --range 0-0 -o /dev/null -w '%{http_code}\n' -K /tmp/rv-par.txt | tally)
echo "   $res"
check "no 429 in 400 parallel pieces" "$(echo "$res" | grep -c 429)" 0
sleep 12

echo "4. spam: the same site, as fast as a loop can"
echo "   4a. 60 requests for paths that are not there (limit 10 per 10 s, 30 per minute)"
res=$(for i in $(seq 60); do code "$SITE/wp-login-$i.php"; echo; done | tally); echo "   $res"
check "404s are limited" "$(echo "$res" | grep -c 429)" 1
echo "   4b. a 429 carries Retry-After and is not cacheable:"
curl -s -D - -o /dev/null "$SITE/x-again" | tr -d '\r' | grep -iE '^(HTTP|retry-after|cache-control)' | sed 's/^/       /'
echo "   4c. 1000 HEAD requests for disk pieces over HTTP/2 (limit 800 per 10 s)"
res=$(head -1 /tmp/rv-pieces.txt | sed "s|.*|url = \"$SITE/disk/$DISK/&\"|" | awk '{for(i=0;i<1000;i++)print}' > /tmp/rv-spam.txt; nullout /tmp/rv-spam.txt; curl -s --parallel --parallel-max 50 -I -o /dev/null -w '%{http_code}\n' -K /tmp/rv-spam.txt | tally); echo "   $res"
check "piece floods are limited" "$(echo "$res" | grep -c 429)" 1
echo "   4d. 60 requests for the manifest (limit 20 per 10 s)"
res=$(for i in $(seq 60); do code "$SITE/disk/$DISK/manifest.json"; echo; done | tally); echo "   $res"
check "manifest floods are limited" "$(echo "$res" | grep -c 429)" 1
echo "   4e. 250 requests for /relay/address, 25 at a time (Caddy's outer limit 100 per 10 s; the relay's own burst of 40 answers 429 first)"
res=$(seq 250 | xargs -P 25 -I{} sh -c "curl -s -o /dev/null -w '%{http_code}\\n' --max-time 30 $SITE/relay/address" | tally); echo "   $res"
check "relay HTTP floods are limited" "$(echo "$res" | grep -c 429)" 1
sleep 12
echo "   4f. 150 WebSocket handshakes, 25 at a time (Caddy's outer limit 60 per 10 s; the relay refuses these, no Origin: 403, then its own 429)"
res=$(seq 150 | xargs -P 25 -I{} sh -c "curl -s -o /dev/null -w '%{http_code}\\n' --max-time 30 --http1.1 -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' '$SITE/relay/?ip=2'" | tally); echo "   $res"
check "WebSocket floods are limited" "$(echo "$res" | grep -c 429)" 1
echo "   4g. 25 POSTs (method not allowed)"
res=$(for i in $(seq 25); do code -X POST -d x "$SITE/"; echo; done | tally); echo "   $res"
check "POST is refused" "$(echo "$res" | grep -c 405)" 1

echo "5. the site answers again after the wait (Retry-After was 10 s for the short windows; the minute windows need up to 60 s)"
sleep 65
check "landing page" "$(code "$SITE/")" 200
check "a disk piece" "$(code --range 0-0 "$SITE/disk/$DISK/$(head -1 /tmp/rv-pieces.txt)")" 206
check "relay address" "$(code "$SITE/relay/address")" 200
check "a missing path is a plain 404 again" "$(code "$SITE/nope")" 404
echo "$pass passed, $fail failed"
rm -f /tmp/rv-manifest.json /tmp/rv-pieces.txt /tmp/rv-load.txt /tmp/rv-par.txt /tmp/rv-spam.txt
[ "$fail" = 0 ]
