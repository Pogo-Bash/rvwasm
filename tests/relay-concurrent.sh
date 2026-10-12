#!/bin/bash
# Several stand-in machines (tests/relay-guest.py) on one relay at once: each gets an address of its own from /address
# (asked the way a page on the relay's own address does, with no Origin header), reaches an allowed name, cannot reach
# the others, and the relay holds the per-address limit; a reload takes a fresh address at once; a machine that goes
# silent without closing its socket is dropped by the relay's pings. Needs root, `ip`, Linux, a relay in allowlist mode
# whose --max-per-ip is 4 (docker compose: RV_MAX_PER_IP=4):
#
#   sudo tests/relay-concurrent.sh [HOST:PORT [ORIGIN]]        (default 127.0.0.1:8081 http://localhost:8080)
set -u
url=${1:-127.0.0.1:8081}
origin=${2:-http://localhost:8080}
here=$(cd "$(dirname "$0")" && pwd)
g() { local ns=$1; shift; python3 "$here/relay-guest.py" --url "$url" --origin "$origin" --same-origin --ns "$ns" "$@"; }
fail=0
check() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fail=1; fi; }
out=$(mktemp -d)
trap 'kill $(jobs -p) 2> /dev/null; sleep 0.5; for n in a b c d e; do ip netns del rvt$n 2> /dev/null; done; rm -rf "$out"' EXIT

# 1. four at once: distinct addresses, each reaches an allowed name, each listens for the others to try
for n in a b c d; do
	g rvt$n -- sh -c 'ip -4 -o addr show eth0 | awk "{print \$4}"; mkdir -p /tmp/www; cd /tmp/www; echo hi > index.html;
		timeout 30 python3 -m http.server 80 --bind $(ip -4 -o addr show eth0 | sed "s,.*inet \([0-9.]*\)/.*,\1,") > /dev/null 2>&1 &
		sleep 4; echo name=$(getent hosts deb.debian.org | head -1 | cut -d" " -f1); curl -m8 -sI https://deb.debian.org/ | head -1;
		me=$(ip -4 -o addr show eth0 | sed "s,.*inet \([0-9.]*\)/.*,\1,");
		for ip in 2 3 4 5 6 7 8 9; do [ 10.0.5.$ip = $me ] || echo peer10.0.5.$ip=$(curl -m2 -s http://10.0.5.$ip/ 2>&1 | head -c 20); done; sleep 12' > "$out/$n" 2>&1 &
	sleep 0.5
done
sleep 10
addrs=$(cat "$out"/[a-d] | grep '^10\.' | sort -u | wc -l)
check "4 machines, 4 distinct addresses" '[ "$addrs" = 4 ]'
check "5th machine from the same address is refused (limit 4)" 'g rvte -- true 2>&1 | grep -q "refused.*429"'
wait
check "every machine got a name from the allowlist DNS" '[ "$(grep -c "^name=[0-9]" "$out"/[a-d] | grep -c ":1$")" = 4 ]'
check "every machine fetched an allowed site" '[ "$(cat "$out"/[a-d] | grep -c "HTTP/.* [23]0")" = 4 ]'
check "no machine reached another" '! cat "$out"/[a-d] | grep "^peer" | grep -v "=$"'

# 2. reload: the new page asks again while the old one is still connected, and does not need the old one's address
g rvta -- sh -c 'ip -4 -o addr show eth0 | awk "{print \$4}"; sleep 15' > "$out/old" 2>&1 &
old=$!
sleep 3
g rvtb -- sh -c 'ip -4 -o addr show eth0 | awk "{print \$4}"; curl -m8 -sI https://deb.debian.org/ | head -1' > "$out/new" 2>&1
check "reload while the old page is still connected gets another address and works" \
	'[ "$(head -1 "$out/old")" != "$(head -1 "$out/new")" ] && grep -q "HTTP/.* [23]0" "$out/new"'
pkill -f "relay-guest.py.*--ns rvta"; wait $old 2> /dev/null; sleep 1

# 3. a machine that is silent (stopped, not closed) is dropped by the relay's pings
[ -z "$(curl -s -H "Origin: $origin" "http://$url/machines")" ] || echo "(machines left over: $(curl -s -H "Origin: $origin" "http://$url/machines"))"
g rvtc -- sleep 120 > /dev/null 2>&1 &
sleep 3
now=$(curl -s -H "Origin: $origin" "http://$url/machines")
pkill -STOP -f "relay-guest.py.*--ns rvtc"
sleep 50
later=$(curl -s -H "Origin: $origin" "http://$url/machines")
check "silent machine is gone after the ping timeout (machines: '$now' -> '$later')" '[ -n "$now" ] && [ -z "$later" ]'
pkill -9 -f "relay-guest.py.*--ns rvtc"
exit $fail
