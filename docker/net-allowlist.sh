#!/bin/bash
# The relay container's network for the public relay (RV_NET_MODE=allowlist, docker/relay.sh): the same TAP device
# and address translation as tools/net-setup.sh, but with a firewall (nftables, this container's own) that lets the
# machines out only to addresses their DNS answers had, on TCP 80 and 443:
#
#   - the machine's only resolver is this container's dnsmasq on 10.0.5.1 (docker/dnsmasq-conf.sh): it answers for
#     the domains in the allowlist and adds the addresses it answered with to the set `allowed4`
#     (dnsmasq --nftset); other DNS servers are not reachable (everything but 10.0.5.1:53 is dropped);
#   - out of the TAP device: new connections to `allowed4` on tcp/80 and tcp/443, nothing else (no other ports, no
#     UDP, no ICMP, no other addresses, no private, loopback, link-local or multicast ranges even if a name resolves
#     into one), and no traffic from one machine to another;
#   - per machine (its source address; the relay does not let a machine use another's): at most RV_CONN_LIMIT
#     connections open, RV_NEW_RATE new ones per second, DNS RV_DNS_RATE queries per second, and the download
#     capped at RV_DOWN_MBYTES MB/s (the upload is limited by the relay itself, tools/net-relay.py).
#
# Run as root, once, in the container. A table of the same name is replaced, so it can be run again.
set -e
TAP=rvtap0
NET=10.0.5.0/24
GATEWAY=10.0.5.1
OWNER="${1:-rv}"
CONN_LIMIT=${RV_CONN_LIMIT:-256}
NEW_RATE=${RV_NEW_RATE:-40}
DNS_RATE=${RV_DNS_RATE:-60}
DOWN_MBYTES=${RV_DOWN_MBYTES:-12}

ip link show "$TAP" > /dev/null 2>&1 || ip tuntap add dev "$TAP" mode tap user "$OWNER"
ip addr show dev "$TAP" | grep -q "$GATEWAY/" || ip addr add "$GATEWAY/24" dev "$TAP"
ip link set "$TAP" up
[ "$(cat /proc/sys/net/ipv4/ip_forward)" = 1 ] || sysctl -q -w net.ipv4.ip_forward=1
# (No reverse-path surprises: a machine's frames come in on the TAP with its own source address only.)

nft delete table inet rvrelay 2> /dev/null || true
nft -f - <<NFT
table inet rvrelay {
	# filled by dnsmasq (nftset=/domain/4#inet#rvrelay#allowed4); an entry lasts a day, longer than any DNS TTL
	set allowed4 {
		type ipv4_addr
		flags timeout
		timeout 1d
		size 65535
	}
	# never reachable, whatever a name resolves to (also: 169.254.169.254, the cloud's metadata service)
	set never4 {
		type ipv4_addr
		flags interval
		elements = { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12,
			192.0.0.0/24, 192.168.0.0/16, 198.18.0.0/15, 224.0.0.0/3 }
	}
	set conns {
		type ipv4_addr
		flags dynamic
		size 1024
	}
	set newrate {
		type ipv4_addr
		flags dynamic
		size 1024
		timeout 5m
	}
	set dnsrate {
		type ipv4_addr
		flags dynamic
		size 1024
		timeout 5m
	}
	set downrate {
		type ipv4_addr
		flags dynamic
		size 1024
		timeout 5m
	}

	chain input {
		type filter hook input priority 0; policy accept;
		# what the machines may ask of this container: DNS from the relay's own address
		iifname "$TAP" ip daddr $GATEWAY udp dport 53 add @dnsrate { ip saddr limit rate over $DNS_RATE/second burst $((DNS_RATE * 2)) packets } drop
		iifname "$TAP" ip daddr $GATEWAY udp dport 53 accept
		iifname "$TAP" ip daddr $GATEWAY tcp dport 53 ct state new add @dnsrate { ip saddr limit rate over $DNS_RATE/second burst $((DNS_RATE * 2)) packets } drop
		iifname "$TAP" ip daddr $GATEWAY tcp dport 53 accept
		iifname "$TAP" ct state established,related accept
		iifname "$TAP" drop
	}

	chain forward {
		type filter hook forward priority 0; policy drop;
		# replies to what a machine started
		oifname "$TAP" ct state established,related add @downrate { ip daddr limit rate over $DOWN_MBYTES mbytes/second } drop
		oifname "$TAP" ct state established,related accept
		iifname "$TAP" ct state established,related accept
		iifname "$TAP" oifname != "$TAP" ip saddr $NET ip daddr @never4 drop
		iifname "$TAP" oifname != "$TAP" ip saddr $NET ip daddr @allowed4 tcp dport { 80, 443 } ct state new add @conns { ip saddr ct count over $CONN_LIMIT } drop
		iifname "$TAP" oifname != "$TAP" ip saddr $NET ip daddr @allowed4 tcp dport { 80, 443 } ct state new add @newrate { ip saddr limit rate over $NEW_RATE/second burst $((NEW_RATE * 2)) packets } drop
		iifname "$TAP" oifname != "$TAP" ip saddr $NET ip daddr @allowed4 tcp dport { 80, 443 } ct state new accept
		# anything else (other ports, other addresses, other machines, UDP, ICMP, IPv6) is dropped by the policy
	}

	chain postrouting {
		type nat hook postrouting priority 100; policy accept;
		iifname "$TAP" oifname != "$TAP" masquerade
	}
}
NFT
echo "$TAP: $GATEWAY/24, owned by $OWNER, forwarding on, firewall: allowlist (nft table inet rvrelay)"
