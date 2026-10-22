#!/bin/bash
# Visitors from the live site's access log (Caddy's, kept 14 days): landing page, machines booted, machines that got
# the network, rate-limited requests, referrers. No browser-side tracking; IPs are counted, not shown.
#   tools/traffic.sh [SERVER] [MINUTES...]     (default: $RV_SERVER, windows 30 360 1440)
set -e
SERVER=${1:-${RV_SERVER:?name the server (an ssh name) as the first argument or in RV_SERVER}}
shift || true
ssh "$SERVER" 'cd ~/rvwasm && docker compose exec -T caddy cat /var/log/caddy/access.log' | python3 -c '
import json, sys, collections
L = [json.loads(l) for l in sys.stdin if l.startswith("{")]
if not L: sys.exit("no log lines")
now = max(x["ts"] for x in L)
ip = lambda x: x["request"].get("client_ip") or x["request"]["remote_ip"]
path = lambda x: x["request"]["uri"].split("?")[0]
def ref(x):
    r = (x["request"]["headers"].get("Referer") or [""])[0]
    return r.split("/")[2] if r.startswith("http") else "(none: apps strip it)"
for m in [int(a) for a in sys.argv[1:]] or [30, 360, 1440]:
    S = [x for x in L if x["ts"] >= now - m * 60]
    land = {ip(x) for x in S if path(x) == "/" and x["status"] == 200}
    boot = {ip(x) for x in S if path(x) == "/desktop"}
    net = {ip(x) for x in S if path(x).startswith("/relay/") and x["status"] == 101}
    refs = collections.Counter(ref(x) for x in S if path(x) == "/" and x["status"] == 200)
    limited = sum(x["status"] == 429 for x in S)
    print(f"last {m} min: {len(land)} visitors, {len(boot)} booted the machine, {len(net)} got the network, "
          f"{limited} rate-limited requests")
    print("   from:", ", ".join(f"{k} {v}" for k, v in refs.most_common(6)))
' "$@"
