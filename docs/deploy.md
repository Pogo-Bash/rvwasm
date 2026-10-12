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

- **Caddy** (`caddy` service, profile `public`; image `rvwasm-caddy:2.10`, stock Caddy plus the rate limiter module, see "Abuse protection") is the only thing that publishes ports (80 and 443, TCP). It gets a
  Let's Encrypt certificate by itself, serves the static files directly (Range requests, ETags; the disk pieces
  are not compressed, the rest is zstd/gzip), and puts `Cross-Origin-Opener-Policy: same-origin`,
  `Cross-Origin-Embedder-Policy: require-corp` and `Cross-Origin-Resource-Policy: same-origin` on every answer: the
  page needs them for shared memory, and only works over HTTPS (or localhost). `.wasm` is served as
  `application/wasm`. The files come from this server, not a CDN (except the disk's pieces, if the manifest says they are on object storage: "Pieces from object storage" below).
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
| machines | every visitor's machine has an address of its own, `10.0.5.N` (N from 2 to 250): before starting the machine the page asks `GET /relay/address` (a plain GET, which a page on the relay's own address sends with no `Origin` header and which the relay therefore answers without one; a WebSocket still needs the allowed origin), takes the number it gets for the kernel's `ip=` and for `wss://DOMAIN/relay/?ip=N`. The relay holds a number it handed out for `RV_ADDRESS_HOLD_SECS` (60 s; limits below), the machine keeps it until it disconnects. A reload asks again and gets another number, so it never waits for the old socket; a page that goes silent without closing (lost network) is dropped after 35 s (the relay pings every 10 s). Machines cannot reach each other: the relay forwards a machine's frames only to the TAP device, the firewall drops traffic from the TAP back to it, and `--strict` stops spoofed sources. (Until 2026-10-12 a same-origin `/address` was refused with 403 for lack of an `Origin`, the page fell back to `ip=2`, and the second visitor got `409 Conflict`: only one visitor at a time had a network.) |
| per visitor | the firewall (nftables, per machine address): at most `RV_CONN_LIMIT` (256) connections open, `RV_NEW_RATE` (40) new ones per second, `RV_DNS_RATE` (60) DNS queries per second, `RV_DOWN_MBYTES` (12) MB/s download. The relay (from Caddy's `X-Forwarded-For`; an IPv6 visitor counts as its /64): `RV_MAX_CLIENTS` (40) machines at once, the sharing and the limits below, and `--strict`: a machine has one hardware address and only sends IPv4 and ARP from the `10.0.5.N` it was given, so it cannot pose as another machine. |

### One address, many people: the network is shared

Many visitors can come from one public address (campus or public wifi), so `RV_MAX_PER_IP` (6) is not a wall: it is how many
machines of one address have the network at once. A machine that comes beyond that is not refused. It connects, gets its address
and *waits*: its IP packets are dropped (ARP still works, so it is ready the moment its turn comes). Then:

- a free place goes to the machine that has waited longest, at once (a machine that disconnects frees one);
- a waiting machine that sends traffic is *needy*. If a machine of the same address that has the network has sent no unicast
  IPv4 packet for `RV_IDLE_SECS` (60) the two change places (the oldest needy one first, the one idle longest loses its place and
  goes to the end of the line); the idle one gets a place back the same way when it sends traffic again;
- a machine that was just given the network keeps it for `RV_HOLD_SECS` (20), so places do not change back and forth;
- at most `RV_MAX_WAITING_PER_IP` (10) wait per address; more get `429`.

The relay tells the page where it stands with a *text* WebSocket message (Ethernet frames are binary ones, so the two cannot be
confused; only a page that connected with `&ctl=1` is sent any):
`{"type":"net-state","state":"active"|"waiting","others":N,"queue":Q,"waiting":W,"cap":C}`. `others`: other machines of this
address that have the network, `queue`: its place in line (0 when it has the network). `dev.html` keeps these messages from
the machine and shows a small banner at the bottom of the page, outside the machine's display ("network relay in use by 2 others
on your network (shared IP). waiting for a free slot, you're next; ..."), which disappears when the machine has the network; it
also logs one line to the browser's console. Nothing is added inside the machine.

### Limits against abuse

Everything a stranger can ask of the relay is limited, so nobody can use it up (all in `.env`, `docker compose up -d` to apply;
`0` turns a limit off; the flags are in `tools/net-relay.py --help`). Over a limit: `429` with `Retry-After` (`503` with it
when the pool of numbers is empty), no crash, no work for the event loop. Tables of counters are bounded (least recently used go
first), so many addresses cannot use memory up either.

| variable (default) | limit |
|---|---|
| `RV_ADDRESS_PER_MIN` (120), `RV_MACHINES_PER_MIN` (30), `RV_WS_PER_MIN` (120), `RV_REQ_BURST` (40) | requests per minute per visitor address (token buckets; the burst is what a lecture hall loading the page at once needs): `GET /address`, `GET /machines`, WebSocket upgrades. Refused and failed attempts (wrong origin, over the limit of machines) count too. |
| `RV_GLOBAL_PER_SEC` (100) | requests per second of all visitors together (burst three seconds' worth) |
| `RV_HANDSHAKE_SECS` (5), `RV_MAX_HANDSHAKES` (100) | seconds a connection has to send its request, and how many may be doing so; requests over 16 KiB are not read |
| `RV_ADDRESS_HOLD_SECS` (60), `RV_HOLDS_MAX` (100), `RV_HOLDS_PER_NET` (24) | numbers handed out by `/address` and not used yet: held this long, at most this many in all and per network (IPv4 /24, IPv6 /64), and for one visitor at most `RV_MAX_PER_IP` + `RV_MAX_WAITING_PER_IP`; the oldest unused hold goes first, so requests alone cannot use up the 249 numbers |
| `RV_MAX_CLIENTS` (40), `RV_MAX_PER_IP` (6), `RV_MAX_WAITING_PER_IP` (10) | machines in all, having the network per address, waiting per address |
| `RV_RATE_FRAMES` (4000), `RV_RATE_BYTES` (6000000) | frames and bytes per second a machine may send (a burst of two seconds' worth) |
| `RV_RELAY_DNS_RATE` (60), `RV_RELAY_SYN_RATE` (40) | DNS queries and new TCP connections per second a machine may make (same burst), counted by the relay on its frames; the firewall has its own (`RV_DNS_RATE`, `RV_NEW_RATE`) |
| `RV_RELAY_DOWN_BYTES` (16000000), `RV_IP_BYTES` (32000000) | bytes per second a machine may receive, and all machines of one address together, both ways (apt needs a few MB/s) |
| `RV_MSG_RATE` (20) | messages per second a page may send that are not frames (it sends none today; pings and text count): more and it is disconnected. Messages over 4096 bytes close the connection. |
| `RV_IDLE_SECS` (60), `RV_HOLD_SECS` (20) | see above |

`GET /relay/machines` tells a visitor about its own machines only (never other visitors' addresses or counts).
`tests/relay-limits.py` (no root, no Docker) and `tests/relay-concurrent.sh` (stand-in machines in network namespaces
against the relay in Docker) check each of these.

What visitors can do: `apt-get install`, `pip install`, `npm install`, `cargo` and `git clone` from GitHub. What
they cannot: browse anywhere else, mail, SSH, game servers, scan.

Limits worth knowing: the firewall can only see addresses, not names, so anything that shares an address with a
listed name is reachable too (GitHub's and Fastly's addresses host many sites; the Debian mirrors sit behind
Fastly). Anything on a listed site can be fetched, and a listed site that lets people upload (GitHub) can be
used to reach whoever hosts there. Keep the list to what is needed.

## Pieces from object storage (Cloudflare R2; optional)

The disk's pieces (3.9 GB, about 4000 files of 1 MiB per boot at the most) can be served by Cloudflare R2 behind a custom
domain instead of this server: the download is then free for the operator, cached at Cloudflare's edge, and the server's
two cores and its line only do the page, the manifest and the relay. **It is optional**: a manifest without `base` makes
the page ask the site itself for the pieces, exactly as it always did, so a self-hosted setup needs no bucket, no account
and none of the settings below.

How it works:

- `tools/split-disk.py` writes the sha256 of every piece into `manifest.json` (`hashes`, one per piece, `""` for a piece
  that is all zeros).
- `tools/r2-publish.py DIST` (the release's `publish` step) hashes the pieces of every disk in `DIST/disk`, makes each one
  an object `disk/<sha256>.bin` (Content-Type `application/octet-stream`, `Cache-Control: public, max-age=31536000,
  immutable`; the extension is there because Cloudflare only caches by extension unless a rule says otherwise), uploads the
  objects the bucket lacks (it lists the bucket once, so a new disk costs only the pieces that changed) and checks the bucket
  holds every one with the right size. Only then it rewrites the manifests, last, adding `base`
  (`https://disk.plasmaontheweb.com/disk/`) and `hashes`. The `id` does not change, so browsers keep their copy of the disk.
  A piece never changes under its name: an update can never serve a stale cached piece, and two disks share the pieces they
  have in common.
- The page (`tools/www/disk-worker.js`) reads `manifest.json` from the site (the manifest is never on the bucket: it is
  what switches the site to a new disk, in one step) and fetches each piece from `base + hash + ".bin"` with a plain
  CORS request, which is also what the page's `Cross-Origin-Embedder-Policy: require-corp` needs for a file from another
  origin. It checks the size. If the bucket does not answer (network error, any status but 200, a wrong size, no CORS
  permission for the page's origin) it fetches the same piece from the site, `disk/NAME/000123`. After three failures in a
  row it leaves the bucket alone for five minutes (one line in the machine's log), so a broken bucket costs a few requests,
  not one per piece. The site therefore must keep its own pieces (`dist/disk/NAME/`): they are the fallback, and
  `tools/deploy.sh` copies them as before. (Ranges are not used: a piece is fetched whole.)
- A refusal from the site is not an I/O error in the machine yet. The worker waits and asks again: for as long as a
  `Retry-After` on a 429 or 503 says (at most 30 s), otherwise 0.5 s doubling to 30 s with jitter, for network errors
  and 5xx too, and gives up (the read fails with EIO) only after two minutes on one piece. A 404 or a piece of the
  wrong size is tried twice. A bucket that answers 429/503 is left alone for its `Retry-After` and the site serves
  meanwhile. (`tests/disk-worker-remote.js` covers these rules.)
- The page's `Content-Security-Policy`: there is none today. If one is added, `connect-src` must include the bucket's domain.

Set up once (Cloudflare):

1. An R2 bucket (`plasmaontheweb`) with a custom domain (`disk.plasmaontheweb.com`; Cloudflare makes the DNS record and the
   certificate). The default `r2.dev` address is rate limited and not for production.
2. A CORS policy on the bucket: allowed origin `https://plasmaontheweb.com` (add the others that serve the site; the
   `sslip.io` name of this server is not allowed, and a page on it falls back to the site's pieces after three tries),
   methods `GET`, `HEAD`, allowed header `Range`, exposed headers `Content-Length`, `Content-Range`, `ETag`.
3. An S3 API token (Object Read & Write, for this bucket). Where it lives: **the Mac only**, in
   `~/.config/rvwasm/r2.env` (mode 600): `R2_ENDPOINT=https://ACCOUNT.r2.cloudflarestorage.com`, `R2_BUCKET`,
   `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`. Never on the server, in the repository, or on the build VM's disk:
   `tools/release-setup.sh` hands the file to the one `publish` command on its standard input, so it is in that process's
   environment only (not in an argument, a file, or `ps`).

Publishing a release (on the Mac): `tools/release-setup.sh` builds in the VM and, because the credentials file exists, ends
with `publish`; `tools/release-setup.sh publish` runs just that (after `dist` made fresh manifests, they have no `base`
until it has run). `RV_PIECE_BASE` overrides the address pieces are fetched from. Then `tools/deploy.sh` copies the dist
to the server: the page and the rest, the pieces of a disk whose `id` the server does not have yet, and the manifests
last (rsync replaces each file in one step; no `--delete`, nothing is built on the server). Check:

    curl -s https://plasmaontheweb.com/disk/release/manifest.json | python3 -c 'import json,sys; m=json.load(sys.stdin); print(m["id"], m["base"], len(m["hashes"]))'
    H=$(curl -s https://plasmaontheweb.com/disk/release/manifest.json | python3 -c 'import json,sys; print([h for h in json.load(sys.stdin)["hashes"] if h][100])')
    curl -sI -H 'Origin: https://plasmaontheweb.com' https://disk.plasmaontheweb.com/disk/$H.bin   # 200, access-control-allow-origin, cf-cache-status HIT on the second request
    node tests/disk-worker-remote.js                                                                # the worker's fallback rules, without a browser

Going back: put a manifest without `base` on the server (take the two keys `base` and `hashes` out of the live one, or
`tools/release-setup.sh dist` and `tools/deploy.sh` make a fresh one), and every browser uses the site's pieces again from
its next boot. The objects stay in the
bucket (they cost storage only; a bucket lifecycle rule can expire `disk/` objects older than the disks in use).

## First deployment (what was done on the server)

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

    tools/deploy.sh [-n] [$HOST] # from the Mac, the usual way: the VM's dist, changed files only (by content), manifests last, no --delete; -n lists what it would copy
    # or by hand, from wherever the dist is (pieces first, the manifests last):
    rsync -a --partial --exclude manifest.json -e ssh DIST/ ubuntu@$HOST:rvwasm/dist/
    rsync -a -e ssh DIST/disk/release/manifest.json ubuntu@$HOST:rvwasm/dist/disk/release/   # (and home)
    # nothing to restart: Caddy reads the files per request, ETags make browsers fetch what changed

`tools/deploy.sh` first brings the build VM's dist up to date with this tree (`docker/patch-dist.sh`, also run by
`docker/release.sh dist`): `dev.html` and `disk-worker.js` are the tree's own `tools/www/` files and the boot image gets
the tree's `/init` appended (the VM may have built the dist from a tree a release behind). Nothing is patched by hand on
the server, and a plain `tools/deploy.sh` gives it exactly this; `tools/deploy.sh -n` lists the files that would change
(`rsync -i`, by content) and copies nothing to the server.

The landing page (`site/`) is mounted into Caddy from the tree: after changing it, rsync `site/` to `~/rvwasm/site/` (nothing
to restart). A new disk (`disk/release/manifest.json` has a new `id`) makes browsers forget the old copy of the disk and fetch the
new one. A changed Caddyfile (limits, headers): copy it to `~/rvwasm/docker/Caddyfile` and `docker compose restart caddy`
(the image is not rebuilt; see "Abuse protection"). To update the server's compose files, Caddyfile or relay scripts: rsync the tree's `docker/`, `site/`,
`tools/net-relay.py`, `tools/net-setup.sh`, `docker-compose.yml` into `~/rvwasm` the same way (`rsync -a --relative`), then `docker compose up -d --build` (the relay restarts: open
machines lose their network for a moment and reconnect only after a reload).

## Abuse protection

Three layers, outermost first: the cloud firewall (only 22, 80 and 443 are open), the host's per-source connection
limits (`docker/host-guard.sh`), Caddy's per-visitor request limits (`docker/Caddyfile`), and then the relay's own
limits (above) for what gets past those. Written for the case that someone points a script (or an AI) at the site
to find every route without a limit and hammer it.

### The routes, what a real visitor does, and the limits

All limits are **per visitor address** (an IPv6 visitor counts as a /64) and sliding windows: "20 per 10 s" means a 21st
request within any 10 seconds is refused. Past a limit the answer is `429 Too Many Requests` with `Retry-After`
(seconds) and `Cache-Control: no-store`; a refused request is not counted, so waiting the `Retry-After` out always
works. Every request that matches several kinds counts in each. Every limit has a short window (the burst of a page
load or a boot) and a long one (what a person can keep up).

| route (what is served) | a real visitor | zone: limit per visitor |
|---|---|---|
| `/`, `/index.html`, `/desktop`, `/dev.html`, `/rv-site.json`, `/robots.txt`, `/sitemap.xml`, `/site.webmanifest` (pages) | 1 to 3 per page load | `pages`: 40 / 10 s, 150 / min |
| landing page files (`site/`: icons, `/assets/*`, `/favicon.ico`) | about 10 per landing page load | `site`: 120 / 10 s, 400 / min |
| boot page files (`dist/www`: the scripts, styles, workers, `rvgpu/`, `vrend/`; no `.wasm`/`.cpio.gz`) | about 30 per boot; the same again, answered 304, per reload | `www`: 400 / 10 s, 1000 / min |
| the big files: `vmlinux-*.wasm` (7 MB), `initramfs-*.cpio.gz` (14 MB), `rvgpu/rvgpu.wasm` (16 MB), `vrend/vrend.wasm` | 4 per boot (once per release; a reload revalidates and gets 304) | `big`: 60 / min, 400 / hour |
| `/disk/NAME/manifest.json` | 1 per disk per boot (2 disks), never polled | `manifest`: 20 / 10 s, 80 / min |
| `/disk/NAME/NNNNNN` (1 MiB pieces) | see below | `piece`: 800 / 10 s, 3000 / min, 10000 / hour |
| `/relay/*` over HTTP (`/relay/address`) | 1 per page load | `relay`: 100 / 10 s, 600 / min |
| `/relay/*` WebSocket handshake (`Upgrade: websocket`) | 1 per page load, then the connection stays | `relay_ws`: 60 / 10 s, 300 / min |
| anything that is not there (404: scanners, `.env`, `wp-login.php`, piece numbers that do not exist) | none | `miss`: 10 / 10 s, 30 / min |
| the whole site, all of the above together | well under | `all`: 1500 / 10 s, 6000 / min |
| port 80 (the redirect to HTTPS) | 1 per first visit | `http`: 60 / 10 s, 300 / min |

Not served at all (404): `RELEASE.txt` and `server.py` in the dist root, anything outside `dist/www`, `dist/disk` and
`site/`, directories (`/disk/`, `/assets/`: no listing, `file_server` has no `browse`). Only `GET` and `HEAD` are
answered (`405` for the rest), a request body may be at most 64 KB (nothing sends one), request headers at most 16 KB.

**The disk pieces, measured.** The machine's disk worker (`tools/www/disk-worker.js`) fetches a piece with a
synchronous request, one at a time, only when the guest reads something that is not in the browser's copy yet, and
keeps it (OPFS) for good. So a visitor's rate is bounded by the line: from a Mac to the server about 2 to 5 pieces a
second (100 pieces, 100 MB, in 18 to 64 s on a home connection); a fast line with a short round trip can reach 30 to 50 a
second; never more, since the requests are not parallel (the limits leave room for several visitors behind one address). A first boot
into the desktop reads a few hundred; everything a person ever does adds to the same copy, and the whole disk is 3948
pieces (3.9 GB), so `10000 / hour` lets an address fetch two whole disks an hour (a few visitors behind one address), and no more. That is also the bandwidth limit: 10 GB an hour per address at most. The `home` disk is 24 pieces, fetched all at once the first time.

**Behind a shared address** (campus or public WiFi, a company, a mobile carrier's NAT) many visitors count as one, so the numbers are sized for a few dozen visitors starting together, not one. The `/relay/*` limits are only a coarse outer flood guard, 3 to 5 times the relay's own per-address limits (`RV_ADDRESS_PER_MIN`, `RV_WS_PER_MIN`, burst 40, global 100/s, which are what a shared address meets first). Tested: 10 stand-in machines (`tests/relay-guest.py`, one address) started at the same moment against the live site all got an address and a connection, no Caddy 429.

### Tuning

The numbers are the `events` lines in the one `rate_limit { }` block of `docker/Caddyfile` (`window` next to them).
After editing, on the server: `docker compose restart caddy` (a second of downtime; the limits' counters start over),
or without a restart `docker compose exec caddy caddy reload --config /etc/caddy/Caddyfile`. What was refused is in
`docker compose logs caddy` (`rate limit exceeded`, with the `zone` and the `remote_ip`); the access log (every
request except disk pieces, with the visitor's address) is `docker compose exec caddy tail -f /var/log/caddy/access.log`
(volume `caddy-logs`, 5 files of 20 MiB, 14 days). A disk piece refused with 429 is a failed read in the visitor's
machine (`disk-worker.js` retries three times at once, then gives an I/O error), so if a real visitor reports disk errors,
raise `piece_*` first. Also in the Caddyfile: `read_header 10s` (slowloris: a connection that does not finish its
request headers in 10 s is closed; checked), `idle 2m`, `write 10m`, `max_header_size 16KB`, `request_body max_size 64KB`.
The WebSocket is not affected by these timeouts (it is hijacked after the handshake); the relay has its own.

Memory: a counter is kept per visitor and zone for its longest window (24 bytes per allowed request), so one visitor
who uses everything costs about 300 KB for an hour, and the module forgets visitors after their windows pass.

### The image

The stock `caddy` image has no rate limiter, so `caddy` runs `rvwasm-caddy:2.10`: Caddy 2.10 with
`github.com/mholt/caddy-ratelimit` (pinned in `docker/Dockerfile.caddy`). `docker compose up --build` does not build it
(`pull_policy: never`; the server has two cores). Build on the Mac, in the Lima VM `rvwasm` (cross-compiled, about 40
seconds, the VM's `docker` needs `sudo`), and load it on the server:

    # in the VM, from docker/ (copy Dockerfile.caddy there):
    sudo docker buildx build --platform linux/amd64 --provenance=false --sbom=false --load -t rvwasm-caddy:2.10 -f Dockerfile.caddy .
    sudo docker save rvwasm-caddy:2.10 | gzip > caddy-img.tgz        # 38 MB
    # then, from the Mac:
    scp caddy-img.tgz $HOST:/tmp/ && ssh $HOST 'gunzip -c /tmp/caddy-img.tgz | docker load && cd rvwasm && docker compose up -d --no-deps caddy'

(`docker compose --profile public up -d` without that image fails with "image not found": load it first. To go back to the
stock Caddy, restore `~/backup-pre-ratelimit/Caddyfile` and `RV_CADDY_IMAGE=caddy:2.10-alpine` in `.env`.)

### Cloudflare, later

Today nothing is in front of Caddy, so the TCP connection's address is the visitor's (checked: the access log shows
real addresses; Docker's NAT keeps them) and the limits use `{client_ip}`. To put Cloudflare in front, uncomment
`trusted_proxies static ...`, `trusted_proxies_strict` and `client_ip_headers CF-Connecting-IP` in the global block of the
Caddyfile (the ranges are of 2026-10-12; refresh them from cloudflare.com/ips-v4 and /ips-v6). The rate limits and
the `X-Forwarded-For` the relay gets (`header_up X-Forwarded-For {client_ip}`: the relay sees one address, the
visitor's, also behind Cloudflare) follow by themselves; tested with a stand-in proxy header, IPv6 included. Then the host
rules below need loosening (every visitor arrives from a Cloudflare address: raise `RV_WEB_RATE`/`RV_WEB_CONNS` a lot or
allow only Cloudflare's ranges and drop the per-source rule) and the cloud security list should accept 80/443 from
Cloudflare's ranges only.

### Host rules (`docker/host-guard.sh`)

Docker publishes 80/443 by NAT, so a visitor's connection is FORWARDed to the Caddy container and never reaches INPUT.
The rules therefore sit in `DOCKER-USER`, the chain Docker leaves to the operator (it is the first thing FORWARD
looks at and Docker never rewrites it), and only for packets that arrive on the server's own interface (`ens3`):
the machines' outgoing traffic through the relay, which crosses FORWARD too, is not touched. The same rules are in
INPUT for a Caddy that would run on the host's network.

| what | rule (per source address) |
|---|---|
| open connections to 80/443 | at most 512 (`connlimit`), the 513th SYN gets a reset (a visitor holds a WebSocket and several fetches; many can share an address) |
| new connections to 80/443 | 50 per second, burst of 200 (`hashlimit`), above that the SYN is dropped. A browser opens 1 to 6 connections for a whole page; a client that opens one per request cannot do more than 50 requests a second. |
| new connections to 22 | 60 per minute, burst of 30; open sessions are never cut |
| SYN floods | `net.ipv4.tcp_syncookies = 1` (kept explicit in `/etc/sysctl.d/60-rvwasm-syn.conf`), and a dropped SYN never creates a conntrack entry (the rules look at NEW packets before they are confirmed) |

Only NEW connections are looked at; established traffic skips the chains. Counters: `sudo sh docker/host-guard.sh status`.
Tested from the Mac (with the first, tighter numbers 20 per second / burst 60 / 128 open; the mechanism is the same): 150 simultaneous new connections to port 80 let 107 through and dropped 43; 140 connections held
open let 128 through and reset the rest; 45 new connections to port 22 in a second let 38 through, and ssh worked
again 35 seconds later. IPv6 has no rules: the server has no IPv6 address (and Docker's userland proxy on `[::]:80` would
hide the visitor's address behind the bridge's, so do not add a global IPv6 address to the server without changing that).

**Persistence.** `/etc/iptables/rules.v4` (loaded by `netfilter-persistent` at boot, before Docker) holds the base rules
(accept 22/80/443, the Oracle `InstanceServices` chain) plus these: the empty `DOCKER-USER` declaration with its one
rule, `RVGUARD-WEB`, `RVGUARD-SSH` and the two jumps in INPUT. Docker's own chains are not in it and must not be:
**never run `netfilter-persistent save`** (it writes everything, Docker's chains included), and never
`netfilter-persistent reload` while Docker runs (the restore flushes the whole filter table, Docker's chains too; restart
Docker afterwards if it was done). Edit the file by hand. `sudo sh docker/host-guard.sh down` removes the rules from the
running system, `up` puts them back (the same numbers; override with `RV_WEB_CONNS`, `RV_WEB_RATE`, `RV_WEB_BURST`,
`RV_SSH_RATE`, `RV_SSH_BURST` in the environment, and edit the matching lines of `rules.v4`). The copy of `rules.v4`
from before is `/etc/iptables/rules.v4.bak-before-rvguard-20261012`; the state before everything is in `~/backup-pre-ratelimit/`.

**Changing firewall rules on that server safely.** Start a timer that undoes the change, apply, test a new ssh
connection, cancel the timer:

    sudo systemd-run --unit=rvguard-revert --on-active=300 /bin/sh /home/ubuntu/rvwasm/docker/host-guard.sh down
    sudo sh docker/host-guard.sh up        # or whatever is being tried
    ssh -o ControlPath=none $HOST true    # from another terminal: a new connection must work
    sudo systemctl stop rvguard-revert.timer

### Test

`tests/caddy-limits.sh [URL]` runs against the live site: a page load, a boot's disk fetch (100 pieces in a row, then 400
requests 8 at a time) never get a 429; spam loops (paths that are not there, piece floods, the manifest, `/relay/address`,
WebSocket handshakes, POSTs) do; the site answers normally after the wait. It sends about 100 MB and a few thousand small
requests over about 2.5 minutes, from one address, so run it alone.

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
    # the rules and limits in one process (no root, no Docker, about two minutes):
    python3 tests/relay-limits.py
    # stand-in machines at once, sharing, a reload, a silent one, the limits (needs root; the relay run with
    # RV_MAX_PER_IP=4 RV_MAX_WAITING_PER_IP=2 RV_IDLE_SECS=8 RV_HOLD_SECS=3, on the local compose stack, not on the server):
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
