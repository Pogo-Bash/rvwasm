#!/bin/bash
# Writes the relay's DNS server configuration (dnsmasq) to $1, from the allowlist file ($RV_ALLOWLIST, one domain
# per line, # comments) when RV_NET_MODE=allowlist, or as a plain forwarder when it is open.
#
# allowlist: no default upstream (no-resolv), so every name is refused (REFUSED) unless it is on the list; for those,
#   the question goes to the container's own resolver (server=/domain/UPSTREAM; RV_DNS_UPSTREAM, else the first
#   nameserver of /etc/resolv.conf: Docker's, 127.0.0.11), and the addresses in the answer are added to the
#   firewall's set (nftset=...), which is the only place the machine may connect to (docker/net-allowlist.sh).
#   (address=/#/ was tried first: dnsmasq 2.91 still forwards what it does not match.)
set -e
out=${1:?usage: dnsmasq-conf.sh OUTFILE}
mode=${RV_NET_MODE:-allowlist}
list=${RV_ALLOWLIST:-/etc/rvwasm/allowlist.txt}
{
	echo "# made by dnsmasq-conf.sh ($mode)"
	echo "keep-in-foreground"
	echo "pid-file="
	echo "listen-address=10.0.5.1"
	echo "bind-interfaces"
	echo "no-hosts"
	echo "domain-needed"
	echo "bogus-priv"
	echo "stop-dns-rebind"
	echo "cache-size=1000"
	echo "dns-forward-max=300"
	echo "edns-packet-max=1232"
	[ "${RV_DNS_LOG:-0}" = 1 ] && { echo "log-queries"; echo "log-facility=-"; }
	if [ "$mode" = allowlist ]; then
		echo "no-resolv"
		up=${RV_DNS_UPSTREAM:-$(awk '$1 == "nameserver" { print $2; exit }' /etc/resolv.conf)}
		[ -n "$up" ] || { echo "dnsmasq-conf: no upstream resolver (RV_DNS_UPSTREAM, /etc/resolv.conf)" >&2; exit 1; }
		n=0
		while IFS= read -r line || [ -n "$line" ]; do
			d=${line%%#*}
			d=$(echo "$d" | tr -d '[:space:]' | tr 'A-Z' 'a-z')
			[ -n "$d" ] || continue
			if ! [[ $d =~ ^([a-z0-9]([a-z0-9-]*[a-z0-9])?\.)+[a-z]{2,}$ ]]; then
				echo "dnsmasq-conf: ignoring '$d' in $list (not a domain name)" >&2
				continue
			fi
			echo "server=/$d/$up"
			echo "nftset=/$d/4#inet#rvrelay#allowed4"
			n=$((n + 1))
		done < "$list"
		echo "# $n domains" >&2
	fi
} > "$out"
