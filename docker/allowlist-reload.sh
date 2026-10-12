#!/bin/bash
# After editing the allowlist (docker compose exec relay /opt/rvwasm/allowlist-reload.sh): the DNS server reads the
# list again. Machines stay connected and open connections go on; the set of allowed addresses starts empty and fills again as the machines look names up.
set -e
/opt/rvwasm/dnsmasq-conf.sh /run/rvrelay/dnsmasq.conf
# (Addresses of domains taken off the list go too; connections already open go on, and the machines' next lookups
# fill the set again.)
nft flush set inet rvrelay allowed4 2> /dev/null || true
pkill -x dnsmasq || true   # (relay.sh starts it again at once, with the new file)
sleep 1
pgrep -x dnsmasq > /dev/null && echo "allowlist: reloaded" || { echo "allowlist: the DNS server did not come back" >&2; exit 1; }
