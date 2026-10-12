#!/bin/bash
# Several stand-in machines (tests/relay-guest.py) on one relay at once: each gets an address of its own from /address
# (asked the way a page on the relay's own address does, with no Origin header), reaches an allowed name, cannot reach
# the others, and the relay holds the per-address limit; a reload takes a fresh address at once; a machine that goes
# silent without closing its socket is dropped by the relay's pings. Then the machines of one address share the network
# (all the stand-ins come from this one): a 5th waits instead of being refused, an idle machine yields to a needy waiting
# one, first come first served, a machine that leaves frees its place at once, no changing back and forth, the control
# messages arrive; and the limits on DNS queries, new connections and requests hold. Needs root, `ip`, Linux, a relay in
# allowlist mode with four places per address, a line of two, a short idle time and hold:
#
#   RV_MAX_PER_IP=4 RV_MAX_WAITING_PER_IP=2 RV_IDLE_SECS=8 RV_HOLD_SECS=3 docker compose up -d relay
#   sudo tests/relay-concurrent.sh [HOST:PORT [ORIGIN]]        (default 127.0.0.1:8081 http://localhost:8080)
#
# (tests/relay-limits.py checks the same rules, and more limits, without root or Docker.)
set -u
url=${1:-127.0.0.1:8081}
origin=${2:-http://localhost:8080}
here=$(cd "$(dirname "$0")" && pwd)
g() { local ns=$1; shift; python3 "$here/relay-guest.py" --url "$url" --origin "$origin" --same-origin --ns "$ns" "$@"; }
fail=0
check() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fail=1; fi; }
out=$(mktemp -d)
trap 'kill $(jobs -p) 2> /dev/null; sleep 0.5; for n in a b c d e f g h; do ip netns del rvt$n 2> /dev/null; done; rm -rf "$out"' EXIT

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
g rvte --ctl-log "$out/ctl-e1" -- sleep 4 > /dev/null 2>&1 &
sleep 2
check "5th machine from the same address is not refused: it waits (told so, first in line)" \
	'grep -q "\"state\":\"waiting\".*\"queue\":1" "$out/ctl-e1"'
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

# 4. sharing: four machines have the network, more wait (the relay: 4 places, a line of 2, idle 8 s, hold 3 s)
# Each stand-in runs a loop that fetches an allowed site when its go-X file appears and writes the first line of the answer
# to res-X; use X asks for one fetch and prints the answer ("none" if there was none: the network is not this machine's).
loop() { echo "while :; do if [ -e $out/go-$1 ]; then rm -f $out/go-$1; r=\$(curl -m3 -sI https://deb.debian.org/ | head -1); echo \"\${r:-none}\" >> $out/res-$1; fi; sleep 0.3; done"; }
use() {
	local n; n=$(cat "$out/res-$1" 2> /dev/null | wc -l); touch "$out/go-$1"
	for _ in $(seq 60); do sleep 0.3; [ "$(cat "$out/res-$1" 2> /dev/null | wc -l)" -gt "$n" ] && break; done
	tail -1 "$out/res-$1"
}
state() { tail -1 "$out/ctl-$1" 2> /dev/null | cut -d' ' -f2-; }
await() {       # await X 'text' SECONDS: until the last control message to X contains text
	for _ in $(seq $(($3 * 5))); do state "$1" | grep -q "$2" && return 0; sleep 0.2; done; return 1
}
flips() {       # the number of times the machines' state changed, not counting the first message
	for f in "$out"/ctl-[a-g]; do [ -e "$f" ] && cut -d' ' -f2- "$f" | sed 's/.*"state":"\([a-z]*\)".*/\1/' | uniq | wc -l | awk '{print $1-1}'; done | awk '{s += $1} END {print s + 0}'
}
for n in a b c d; do g rvt$n --ctl-log "$out/ctl-$n" -- sh -c "$(loop $n)" > /dev/null 2>&1 & sleep 0.3; done
sleep 4
for n in a b c d; do res=$(use $n); echo "use $n: $res"; done > "$out/uses"
check "four machines of one address have the network and are told so" \
	'[ "$(grep -c "HTTP/.* [23]0" "$out/uses")" = 4 ] && for n in a b c d; do state $n | grep -q "\"state\":\"active\"" || exit 1; done'
g rvte --ctl-log "$out/ctl-e" -- sh -c "$(loop e)" > /dev/null 2>&1 &
sleep 2
check "the fifth connects, waits, is told its place and how many use the network" \
	'state e | grep -q "\"state\":\"waiting\".*\"others\":4.*\"queue\":1"'
check "... and its traffic is dropped while every machine ahead of it is busy (not yet idle)" '[ "$(use e)" = none ]'
check "... the others are told how many wait" 'state a | grep -q "\"waiting\":1"'
check "an idle machine yields to the needy waiting one (within the idle time and a tick)" 'await e "\"state\":\"active\"" 20'
check "... the one that had been idle longest is the one that waits, first in line" 'await a "\"state\":\"waiting\".*\"queue\":1" 3'
check "... the others keep the network" 'state b | grep -q "\"state\":\"active\"" && state d | grep -q "\"state\":\"active\""'
check "... and the new owner can fetch at once (it was waiting with ARP resolved)" 'use e | grep -q "HTTP/.* [23]0"'
g rvtf --ctl-log "$out/ctl-f" -- sh -c "$(loop f)" > /dev/null 2>&1 &
sleep 2
check "a sixth waits behind the first, in order of arrival (place 2)" 'state f | grep -q "\"queue\":2"'
check "the line is bounded: a seventh from the address is refused (429)" 'g rvtg -- true 2>&1 | grep -q "refused.*429"'
kill $(pgrep -f "relay-guest.py.*--ns rvtb" | head -1) 2> /dev/null
check "a machine that disconnects frees its place at once for the one that waited longest" \
	'await a "\"state\":\"active\"" 3 && await f "\"queue\":1" 3'
check "... the newly given place works" 'use a | grep -q "HTTP/.* [23]0"'
check "f (next in line) needs the network, c and d are idle: it takes the place of the one idle longest at once" \
	'use f | grep -q "HTTP/.* [23]0" && await f "\"state\":\"active\"" 3 && await c "\"state\":\"waiting\"" 3 && state d | grep -q "\"state\":\"active\""'
check "no changing back and forth: five changes of state in all (e in, a out, a in, f in, c out)" '[ "$(flips)" = 5 ]'
check "every control message is the documented JSON" '! cat "$out"/ctl-? | cut -d" " -f2- | grep -qv "^{\"type\":\"net-state\",\"state\":\"[a-z]*\",\"others\":[0-9]*,\"queue\":[0-9]*,\"waiting\":[0-9]*,\"cap\":4}$"'
pkill -f "relay-guest.py.*--ns rvt[a-g]"; sleep 2

# 5. the limits of a machine: DNS queries and new connections per second
flood='
import socket, struct, time, sys
host = socket.gethostbyname("deb.debian.org")        # (the machine'"'"'s resolver: it also lets the firewall know the address)
def q(i, name=b"deb.debian.org"):
    return struct.pack(">HHHHHH", i, 0x0100, 1, 0, 0, 0) + b"".join(bytes([len(p)]) + p for p in name.split(b".")) + b"\0\0\1\0\1"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setblocking(False)
t = time.time()
for i in range(600):
    s.sendto(q(i), ("10.0.5.1", 53))
got = 0
while time.time() < t + 3:
    try: s.recv(512); got += 1
    except BlockingIOError: time.sleep(0.01)
print("dns", got)
import selectors
sel = selectors.DefaultSelector(); socks = []
for i in range(600):
    c = socket.socket(); c.setblocking(False)
    try: c.connect((host, 443))
    except BlockingIOError: pass
    socks.append(c); sel.register(c, selectors.EVENT_WRITE)
up = 0; t = time.time()
done = set()
while time.time() < t + 3 and len(done) < 600:
    for key, _ in sel.select(0.2):
        if key.fileobj not in done:
            done.add(key.fileobj)
            if key.fileobj.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR) == 0: up += 1
print("connect", up)
'
g rvta -- python3 -c "$flood" > "$out/flood" 2>&1
dnsgot=$(awk '$1=="dns"{print $2}' "$out/flood"); conngot=$(awk '$1=="connect"{print $2}' "$out/flood")
check "600 DNS queries in a burst: a limited number is answered (got $dnsgot; the relay's burst is two seconds' worth)" '[ -n "$dnsgot" ] && [ "$dnsgot" -lt 400 ] && [ "$dnsgot" -gt 20 ]'
check "600 connection attempts in a burst: a limited number is made (got $conngot)" '[ -n "$conngot" ] && [ "$conngot" -lt 300 ] && [ "$conngot" -gt 5 ]'

# 6. the limits on requests (last: they use up this address's share for a minute)
for _ in $(seq 300); do curl -s -w '|%{http_code}\n' "http://$url/address"; done > "$out/hammer"
codes=$(cut -d'|' -f2 "$out/hammer" | sort | uniq -c | tr '\n' ' ')
check "/address hammered 300 times from one address: served, then 429 ($codes)" 'echo "$codes" | grep -q "429" && echo "$codes" | grep -q " 200 "'
check "... with Retry-After" 'for _ in $(seq 20); do curl -si "http://$url/address"; done | grep -qi "^retry-after: [0-9]"'
numbers=$(grep '|200$' "$out/hammer" | cut -d'|' -f1 | sort -u | wc -l)
check "... and the numbers held for one address are few ($numbers different ones, at most 16)" '[ "$numbers" -le 16 ] && [ "$numbers" -ge 1 ]'
codes=$(for _ in $(seq 100); do curl -s -o /dev/null -w "%{http_code}\n" "http://$url/machines"; done | sort | uniq -c | tr '\n' ' ')
check "/machines hammered: 429 too ($codes)" 'echo "$codes" | grep -q "429"'
codes=$(for _ in $(seq 300); do curl -s -o /dev/null -w "%{http_code}\n" -H "Origin: http://evil.example" -H "Upgrade: websocket" -H "Connection: Upgrade" -H "Sec-WebSocket-Key: x" -H "Sec-WebSocket-Version: 13" "http://$url/"; done | sort | uniq -c | tr '\n' ' ')
check "refused WebSocket attempts count too: 403, then 429 ($codes)" 'echo "$codes" | grep -q "429" && echo "$codes" | grep -q " 403"'
exit $fail
