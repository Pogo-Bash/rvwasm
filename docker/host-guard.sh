#!/bin/sh
# The public server's host-level limits (docs/deploy.md "Abuse protection"): per-source limits on new connections to
# the site (80/443) and on SSH (22). Run on the server, as root.
#
#   host-guard.sh up       apply now (idempotent); what netfilter-persistent loads at boot is /etc/iptables/rules.v4,
#                          which holds the same rules (the block between "rvguard" markers there)
#   host-guard.sh down     remove them again (also what the safety timer does when a change is being tried)
#   host-guard.sh status   show the chains with their counters (packets dropped so far)
#
# Where the rules sit: Docker publishes 80/443 with NAT, so a visitor's connection to Caddy is FORWARDed to the Caddy
# container, never INPUT. Docker leaves one chain to the operator for that, DOCKER-USER (the first thing FORWARD
# looks at, and Docker never rewrites it); the rules go there, and only for packets that come in on the server's
# network interface, so the machines' own outgoing traffic through the relay (which also crosses FORWARD, to port
# 80/443 of the internet) is not touched. INPUT gets the same for the case that Caddy ever runs on the host's network.
# SSH is INPUT.
#
# The numbers (per source address; an IPv6 address would be a /64, but the server has no IPv6 address):
#   web  at most 512 connections open at once, new ones 50 per second (a burst of 200); beyond that the SYN is dropped
#        (a browser opens 1 to 6 connections to a site; a loop that opens one per request does 50 a second at most; many visitors can share one address, each holding a WebSocket and parallel fetches)
#   ssh  new connections 60 per minute (a burst of 30)
# Only NEW connections are looked at: nothing that is already open is ever cut, and ESTABLISHED traffic skips these chains.
set -e
EXT=${RV_EXT_IF:-$(ip -o -4 route show default | awk '{print $5; exit}')}
WEB_CONNS=${RV_WEB_CONNS:-512}
WEB_RATE=${RV_WEB_RATE:-50/second}
WEB_BURST=${RV_WEB_BURST:-200}
SSH_RATE=${RV_SSH_RATE:-60/minute}
SSH_BURST=${RV_SSH_BURST:-30}

ipt() { iptables -w "$@"; }
# add a rule only when it is not there yet
ensure() { ipt -C "$@" 2>/dev/null || ipt -A "$@"; }
ensure_first() { chain=$1; shift; ipt -C "$chain" "$@" 2>/dev/null || ipt -I "$chain" 1 "$@"; }

case "${1:-}" in
up)
  [ -n "$EXT" ] || { echo "no default interface found (RV_EXT_IF=...)" >&2; exit 1; }
  ipt -N DOCKER-USER 2>/dev/null || true
  ipt -N RVGUARD-WEB 2>/dev/null || ipt -F RVGUARD-WEB
  ipt -N RVGUARD-SSH 2>/dev/null || ipt -F RVGUARD-SSH
  ipt -A RVGUARD-WEB -p tcp -m connlimit --connlimit-above "$WEB_CONNS" --connlimit-mask 32 -j REJECT --reject-with tcp-reset
  ipt -A RVGUARD-WEB -m hashlimit --hashlimit-name rv_web_new --hashlimit-mode srcip --hashlimit-srcmask 32 \
      --hashlimit-above "$WEB_RATE" --hashlimit-burst "$WEB_BURST" --hashlimit-htable-max 32768 --hashlimit-htable-expire 60000 -j DROP
  ipt -A RVGUARD-WEB -j RETURN
  ipt -A RVGUARD-SSH -m hashlimit --hashlimit-name rv_ssh_new --hashlimit-mode srcip --hashlimit-srcmask 32 \
      --hashlimit-above "$SSH_RATE" --hashlimit-burst "$SSH_BURST" --hashlimit-htable-max 4096 --hashlimit-htable-expire 120000 -j DROP
  ipt -A RVGUARD-SSH -j RETURN
  # forwarded to the Caddy container (DNAT): from outside, to 80/443, new connections
  ensure_first DOCKER-USER -i "$EXT" -p tcp -m multiport --dports 80,443 -m conntrack --ctstate NEW -j RVGUARD-WEB
  # to the host itself (the loopback accept is above this in INPUT; the existing accept rules for 22/80/443 come below)
  if ! ipt -C INPUT -p tcp -m multiport --dports 80,443 -m conntrack --ctstate NEW -j RVGUARD-WEB 2>/dev/null; then
    n=$(ipt -L INPUT --line-numbers -n | awk '$2=="ACCEPT" && /dpt:22/ {print $1; exit}')
    ipt -I INPUT "${n:-1}" -p tcp -m multiport --dports 80,443 -m conntrack --ctstate NEW -j RVGUARD-WEB
  fi
  if ! ipt -C INPUT -p tcp --dport 22 -m conntrack --ctstate NEW -j RVGUARD-SSH 2>/dev/null; then
    n=$(ipt -L INPUT --line-numbers -n | awk '$2=="ACCEPT" && /dpt:22/ {print $1; exit}')
    ipt -I INPUT "${n:-1}" -p tcp --dport 22 -m conntrack --ctstate NEW -j RVGUARD-SSH
  fi
  ;;
down)
  while ipt -D DOCKER-USER -i "$EXT" -p tcp -m multiport --dports 80,443 -m conntrack --ctstate NEW -j RVGUARD-WEB 2>/dev/null; do :; done
  while ipt -D INPUT -p tcp -m multiport --dports 80,443 -m conntrack --ctstate NEW -j RVGUARD-WEB 2>/dev/null; do :; done
  while ipt -D INPUT -p tcp --dport 22 -m conntrack --ctstate NEW -j RVGUARD-SSH 2>/dev/null; do :; done
  for c in RVGUARD-WEB RVGUARD-SSH; do ipt -F $c 2>/dev/null && ipt -X $c 2>/dev/null || true; done
  ;;
status)
  ipt -L DOCKER-USER -n -v --line-numbers
  ipt -L INPUT -n -v --line-numbers
  ipt -L RVGUARD-WEB -n -v
  ipt -L RVGUARD-SSH -n -v
  ;;
*) echo "usage: $0 up|down|status" >&2; exit 2 ;;
esac
