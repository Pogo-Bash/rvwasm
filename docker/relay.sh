#!/bin/bash
# The relay container (docker-compose.yml): the TAP device and the address translation, in this container's own
# network, a DNS server on the relay's address, then tools/net-relay.py as an ordinary user.
#
# RV_NET_MODE:
#   allowlist (default)  the public relay: docker/net-allowlist.sh's firewall (TCP 80/443 to the addresses the
#                        allowlist's domains resolved to, nothing else), dnsmasq answering for those domains only
#                        (docker/dnsmasq-conf.sh, the list in /etc/rvwasm/allowlist.txt), and the relay in strict
#                        mode with limits on machines and traffic
#   open                 tools/net-setup.sh: the machine reaches the whole internet (a relay on your own computer);
#                        dnsmasq forwards everything
#
# The relay listens on all of the container's addresses: the container's port is published on the
# host's 127.0.0.1 only (docker-compose.yml), which is what --bind 127.0.0.1 does outside a container.
# It takes connections from the page only: http://localhost:PORT and http://127.0.0.1:PORT, PORT being the
# port the page is served on (RV_HTTP_PORT), plus any origins in RV_ORIGINS (separated by spaces: for the public
# page, https://DOMAIN).
set -e
mode=${RV_NET_MODE:-allowlist}
mkdir -p /run/rvrelay
case $mode in
allowlist) /opt/rvwasm/net-allowlist.sh rv ;;
open) /opt/rvwasm/net-setup.sh rv ;;
*) echo "relay: RV_NET_MODE must be allowlist or open, not '$mode'" >&2; exit 1 ;;
esac

# The machine's DNS server, started again if it stops (allowlist-reload.sh stops it to have it read the list again).
/opt/rvwasm/dnsmasq-conf.sh /run/rvrelay/dnsmasq.conf
(while :; do dnsmasq --conf-file=/run/rvrelay/dnsmasq.conf || true; sleep 1; done) &

origins=()
for o in "http://localhost:${RV_HTTP_PORT:-8080}" "http://127.0.0.1:${RV_HTTP_PORT:-8080}" ${RV_ORIGINS-}; do
	origins+=(--origin "$o")
done
limits=()
if [ "$mode" = allowlist ]; then
	# (every number is an RV_* variable, docker-compose.yml passes them; docs/deploy.md says what each limit is)
	limits=(--strict --trust-forwarded --max-clients "${RV_MAX_CLIENTS:-40}"
		--max-per-ip "${RV_MAX_PER_IP:-6}" --max-waiting-per-ip "${RV_MAX_WAITING_PER_IP:-10}"
		--idle-secs "${RV_IDLE_SECS:-60}" --hold-secs "${RV_HOLD_SECS:-20}"
		--rate-frames "${RV_RATE_FRAMES:-4000}" --rate-bytes "${RV_RATE_BYTES:-6000000}"
		--dns-rate "${RV_RELAY_DNS_RATE:-60}" --syn-rate "${RV_RELAY_SYN_RATE:-40}"
		--down-bytes "${RV_RELAY_DOWN_BYTES:-16000000}" --ip-bytes "${RV_IP_BYTES:-32000000}"
		--address-per-min "${RV_ADDRESS_PER_MIN:-120}" --machines-per-min "${RV_MACHINES_PER_MIN:-30}"
		--ws-per-min "${RV_WS_PER_MIN:-120}" --burst "${RV_REQ_BURST:-40}" --global-per-sec "${RV_GLOBAL_PER_SEC:-100}"
		--handshake-secs "${RV_HANDSHAKE_SECS:-5}" --max-handshakes "${RV_MAX_HANDSHAKES:-100}"
		--address-hold-secs "${RV_ADDRESS_HOLD_SECS:-60}" --holds-max "${RV_HOLDS_MAX:-100}"
		--holds-per-net "${RV_HOLDS_PER_NET:-24}" --msg-rate "${RV_MSG_RATE:-20}")
fi
exec setpriv --reuid=rv --regid=rv --init-groups python3 -u /opt/rvwasm/net-relay.py --bind 0.0.0.0 --port 8001 "${limits[@]}" "${origins[@]}"
