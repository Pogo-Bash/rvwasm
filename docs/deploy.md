# Deploying rvwasm publicly

How the public site is run: one small server, Docker Compose, Caddy for HTTPS, a restricted network relay. The
machine itself runs in each visitor's browser; the server hands out files and the relay.

**Operator's responsibility.** The relay is how the visitors' machines reach the internet, and from the internet
the traffic comes from *this server's address*. Whoever runs the server is responsible for that traffic. The
relay therefore only allows what the allowlist lists (below) and limits what one visitor can do, but it does not
make the operator unaccountable: keep the list short, read the relay's log (`docker compose logs relay`) when
something looks wrong, and take the site down (`docker compose --profile public stop caddy`) if it is abused. A
relay that you run on your own computer for yourself can be opened up (`RV_NET_MODE=open`); do not do that on a
public server.

## What is deployed

    visitor's browser
        │ HTTPS (443; 80 redirects)           Caddy  (docker/Caddyfile)
        ├───────────────────────────────────►  /            the page, kernel, boot image   (files from ./dist/www)
        │                                      /disk/...     the disk's 1 MiB pieces        (files from ./dist/disk)
        │                                      /rv-site.json what the page takes from the server
        │ WSS  /relay/                         /relay/*  ──►  relay:8001  (tools/net-relay.py)
        │                                                          │ TAP rvtap0 10.0.5.1/24 (inside the relay container)
        │                                                          ├─ dnsmasq on 10.0.5.1: answers for allowlisted names
        │                                                          └─ nftables: out only to those addresses, tcp 80/443
        └ (the machine's whole network is that one WebSocket)                         │ NAT
                                                                                      ▼ the internet

- **Caddy** (`caddy` service, profile `public`) is the only thing that publishes ports (80 and 443, TCP). It gets a
  Let's Encrypt certificate by itself, serves the static files directly (Range requests, ETags; the disk pieces
  are not compressed, the rest is zstd/gzip), and puts `Cross-Origin-Opener-Policy: same-origin`,
  `Cross-Origin-Embedder-Policy: require-corp` and `Cross-Origin-Resource-Policy: same-origin` on every answer: the
  page needs them for shared memory, and only works over HTTPS (or localhost). `.wasm` is served as
  `application/wasm`. The files come from this server, not a CDN.
- **relay** is `docker/relay.sh` in `RV_NET_MODE=allowlist` (the default). It is not reachable from outside the
  server except through Caddy's `/relay/`, and it accepts WebSockets only from `https://$RV_DOMAIN` (plus
  `RV_ORIGINS`).
- **www** (`serve.py`) is part of the local setup; on the public server nothing talks to it (Caddy serves the files),
  it only listens on 127.0.0.1.

### The restricted relay

| what | how |
|---|---|
| DNS | the machine's resolver is the relay's dnsmasq (`10.0.5.1`; the page adds `dns=10.0.5.1` to the kernel command line, `/init` writes it into `/etc/resolv.conf`). dnsmasq has no default upstream: a name that is not on the list gets `REFUSED`; a listed name (and its subdomains) is asked of the container's resolver. |
| addresses | dnsmasq adds every address it answers for a listed name to the nftables set `allowed4` (`--nftset`, an entry lasts a day). |
| firewall | `docker/net-allowlist.sh`: from the machines only new TCP connections to `allowed4` on ports 80 and 443 are forwarded. Everything else is dropped: other ports, UDP, ICMP, other addresses, any other DNS server (UDP and TCP 53 are only answered by `10.0.5.1`), private, loopback, link-local (the cloud metadata service) and multicast ranges even if a listed name resolves into one, other machines, IPv6. |
| machines | every visitor's machine has an address of its own, `10.0.5.N` (N from 2 to 250): before starting the machine the page asks `GET /relay/address` (a plain GET, which a page on the relay's own address sends with no `Origin` header and which the relay therefore answers without one; a WebSocket still needs the allowed origin), takes the number it gets for the kernel's `ip=` and for `wss://DOMAIN/relay/?ip=N`. The relay holds a number it handed out for 5 minutes (at most 4 for one visitor), the machine keeps it until it disconnects. A reload asks again and gets another number, so it never waits for the old socket; a page that goes silent without closing (lost network) is dropped after 35 s (the relay pings every 10 s). Machines cannot reach each other: the relay forwards a machine's frames only to the TAP device, the firewall drops traffic from the TAP back to it, and `--strict` stops spoofed sources. (Until 2026-10-12 a same-origin `/address` was refused with 403 for lack of an `Origin`, the page fell back to `ip=2`, and the second visitor got `409 Conflict`: only one visitor at a time had a network.) |
| per visitor | at most `RV_CONN_LIMIT` (256) connections open, `RV_NEW_RATE` (40) new ones per second, `RV_DNS_RATE` (60) DNS queries per second, `RV_DOWN_MBYTES` (12) MB/s download (nftables, per machine address). The relay itself: `RV_MAX_CLIENTS` (40) machines at once, `RV_MAX_PER_IP` (2) per visitor address (from Caddy's `X-Forwarded-For`), `RV_RATE_FRAMES`/`RV_RATE_BYTES` (4000 frames/s, 6 MB/s) upload, and `--strict`: a machine has one hardware address and only sends IPv4 and ARP from the `10.0.5.N` it was given, so it cannot pose as another machine. |

What visitors can do: `apt-get install`, `pip install`, `npm install`, `cargo` and `git clone` from GitHub. What
they cannot: browse anywhere else, mail, SSH, game servers, scan.

Limits worth knowing: the firewall can only see addresses, not names, so anything that shares an address with a
listed name is reachable too (GitHub's and Fastly's addresses host many sites; the Debian mirrors sit behind
Fastly). Anything on a listed site can be fetched, and a listed site that lets people upload (GitHub) can be
used to reach whoever hosts there. Keep the list to what is needed.

## First deployment (an example)

On the server (Ubuntu 24.04, Docker from Docker's own apt repository, `ubuntu` in the `docker` group, ports 80/443
open in iptables and in the cloud firewall):

    mkdir -p ~/rvwasm && cd ~/rvwasm
    # from the Mac (see "Updating" below): the tree's docker/, site/, tools/net-*.py, tools/initramfs-init,
    # tools/www/dev.html, blenderweb/ (the image's build context needs it), docker-compose.yml, and the dist in dist/
    cat > .env <<'ENV'
    RV_DOMAIN=plasmaontheweb.com
    RV_WWW_DOMAIN=www.plasmaontheweb.com
    RV_DIST=/home/ubuntu/rvwasm/dist
    COMPOSE_PROFILES=public
    ENV
    docker/patch-dist.sh dist          # only for a dist built before the allowlist relay
    docker compose up -d --build

`plasmaontheweb.com` is the site and `www.` redirects to it (`RV_ALT_DOMAINS` and `RV_ORIGINS` can name more
addresses that serve the same site; the main domain's origin follows `RV_DOMAIN` by itself).

`COMPOSE_PROFILES=public` is what makes plain `docker compose up -d` start Caddy too. The first start builds the small
`rvwasm-run` image (Debian with Python, dnsmasq and nftables) and pulls Caddy; nothing big is built on the server.
Nothing else is published: `docker compose ps` shows 80 and 443 for Caddy and 127.0.0.1 for www and relay.

## Switching to another domain

1. Point the new domain's A record at the server's address (the server's public address). Wait until
   `dig +short NEWDOMAIN @1.1.1.1` shows it: Let's Encrypt asks from outside, and failed attempts count against its limits.
2. In `~/rvwasm/.env` set `RV_DOMAIN=NEWDOMAIN` (the one setting; the relay's accepted origin follows it). Keep
   `RV_ALT_DOMAINS` (more names that serve the same site, e.g. an sslip.io name for the server's address; add their `https://` origins to
   `RV_ORIGINS`) or empty it. `RV_WWW_DOMAIN=www.NEWDOMAIN` makes that name redirect to the apex, once its record exists.
3. `docker compose up -d`. Caddy gets the certificate on the first request (a few seconds); the certificates are
   kept in the `caddy-data` volume.

## Editing the allowlist

`docker/allowlist.txt` in the server's copy of the tree: one domain per line (`#` comments), a domain includes its
subdomains. Then

    docker compose exec relay /opt/rvwasm/allowlist-reload.sh

dnsmasq reads the list again, the set of allowed addresses is emptied and fills again as the machines look names
up (a machine that resolved a name a minute ago and connects again may need to look it up again). No restart,
machines stay connected. `RV_DNS_LOG=1` in `.env` (then `docker compose up -d`) logs every DNS query to the relay's log:
useful to see what a visitor's machine needs that the list lacks.

## Updating the dist

The dist (`dist/www`, `dist/disk/...`, about 4 GB) is built elsewhere (`docker/release.sh`, the build VM) and only
copied here:

    rsync -a --partial --delete -e ssh DIST/ ubuntu@SERVER:rvwasm/dist/
    ssh SERVER 'cd rvwasm && docker/patch-dist.sh dist'     # only for a dist built before the allowlist relay
    # nothing to restart: Caddy reads the files per request, ETags make browsers fetch what changed

The landing page (`site/`) is mounted into Caddy from the tree: after changing it, rsync `site/` to `~/rvwasm/site/` (nothing
to restart). A new disk (`disk/release/manifest.json` has a new `id`) makes browsers forget the old copy of the disk and fetch the
new one. To update the server's compose files, Caddyfile or relay scripts: rsync the tree's `docker/`, `site/`,
`tools/net-relay.py`, `tools/net-setup.sh`, `docker-compose.yml` into `~/rvwasm` the same way (`rsync -a --relative`), then `docker compose up -d --build` (the relay restarts: open
machines lose their network for a moment and reconnect only after a reload).

## Cloud firewall (Oracle Cloud)

The server's own iptables allows 22, 80 and 443 (saved with `netfilter-persistent`). Oracle Cloud also filters in front
of the instance, and that is set in the console, not on the server: **Networking > Virtual cloud networks > the
instance's VCN > Subnets > the instance's subnet > Security Lists > Default Security List > Add Ingress Rules**
(or the instance's network security group, if it uses one): Source CIDR `0.0.0.0/0`, IP protocol TCP, destination
port `80`; the same again for `443`. (Stateful rules; no egress change is needed.) Without these two rules the site
is not reachable from outside, although the server itself answers.

## Checks

    curl -sI https://DOMAIN/                  # 302/200 with cross-origin-opener-policy and -embedder-policy
    curl -s  https://DOMAIN/rv-site.json      # {"net_path": "/relay", "dns": "10.0.5.1", ...}
    curl -s https://DOMAIN/relay/address       # a number: the relay answers (each call another one, held for 5 minutes)
    docker compose exec relay nft list ruleset | less                       # the firewall and the allowed addresses
    # four stand-in machines at once, a reload, a silent one (needs root; the relay with RV_MAX_PER_IP=4):
    sudo tests/relay-concurrent.sh 127.0.0.1:8081 http://localhost:8080
    # from a stand-in machine in the build VM (tests/relay-guest.py, needs root): names, addresses and ports
    sudo tests/relay-guest.py --url 127.0.0.1:8081 --origin http://localhost:8080 -- sh -c \
      'dig @10.0.5.1 +short deb.debian.org; dig @10.0.5.1 example.com | grep status; curl -m5 https://1.1.1.1/'

## The server (notes)

The public instance runs on a small Oracle Cloud VM (Ubuntu 24.04, 2 cores, 15 GB RAM). Docker comes from Docker's apt
repository (log files rotated at 3 x 10 MB, `/etc/docker/daemon.json`). iptables (`/etc/iptables/rules.v4`, loaded by
`netfilter-persistent`) accepts 22, 80 and 443; Docker's own chains are not saved in that file, Docker adds them at
start. Docker publishes 80 and 443 through its own NAT rules, so the host's INPUT rules for them matter only for
non-Docker listeners; the cloud security list is what opens them to the internet.
