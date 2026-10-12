#!/bin/bash
# The build VM's side of the Linux/Wasm system's network (see tools/net-relay.py):
# a TAP device that the relay reads and writes, an address on it, and
# forwarding with address translation from its network to everywhere else.
# Run with sudo, once per boot of the VM; running it again changes nothing.
#
#   sudo tools/net-setup.sh [USER]     USER: who runs the relay (default: who called sudo)

set -e

TAP=rvtap0
NET=10.0.5.0/24
GATEWAY=10.0.5.1
OWNER="${1:-${SUDO_USER:-$USER}}"

ip link show "$TAP" > /dev/null 2>&1 || ip tuntap add dev "$TAP" mode tap user "$OWNER"
ip addr show dev "$TAP" | grep -q "$GATEWAY/" || ip addr add "$GATEWAY/24" dev "$TAP"
ip link set "$TAP" up
# (Only if it is off: in a container /proc/sys cannot be written, and forwarding is switched on from outside.)
[ "$(cat /proc/sys/net/ipv4/ip_forward)" = 1 ] || sysctl -q -w net.ipv4.ip_forward=1

rule() { # table, chain, rule...: add the rule unless it is there
	local table=$1 chain=$2
	shift 2
	iptables -t "$table" -C "$chain" "$@" 2> /dev/null || iptables -t "$table" -I "$chain" "$@"
}
rule nat POSTROUTING -s "$NET" ! -d "$NET" -j MASQUERADE
# Where Docker is installed the FORWARD chain drops by default; its DOCKER-USER chain is consulted first.
chain=FORWARD
iptables -L DOCKER-USER > /dev/null 2>&1 && chain=DOCKER-USER
rule filter "$chain" -i "$TAP" -j ACCEPT
rule filter "$chain" -o "$TAP" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT

echo "$TAP: $GATEWAY/24, owned by $OWNER, forwarding on"
