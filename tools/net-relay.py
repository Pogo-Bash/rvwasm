#!/usr/bin/env python3
"""The network behind the Linux/Wasm system's interface.

A WebSocket server on which every binary message is one Ethernet frame, and a
TAP device in this machine that the frames go to and come from. With the
routing tools/net-setup.sh sets up, the TAP device is a small network whose
gateway (this machine) forwards to the real one, so the system in the browser
has an ordinary IP network: an address on 10.0.5.0/24, gateway 10.0.5.1.

More than one page can be connected (two tabs, a test beside a person). Each
is a machine of its own on that network, so each needs an address of its own:
a page asks for one before it starts its machine (GET /address answers with
the last number of a free one, from 2 up) and says which it took when it
connects (ws://...:8001/?ip=N); the address is its machine's until it
disconnects. A page that reloads asks again and gets a fresh one, never the
dying page's, whose socket may take a while to go: the relay pings its pages
and drops one that has been silent for PING_DEAD seconds.
(/address and /machines are plain GETs and take no Origin: a page on the
relay's own address, behind a reverse proxy, sends none for a request to its
own origin, and was refused, took 2 and collided with every other visitor.
Addresses handed out and not yet used are held for RESERVE seconds, at most
RESERVE_PER_IP for one visitor, so one visitor cannot use them all up.)
And each gets only the frames that are for it: this is a switch,
which learns from the frames a machine sends which hardware address is whose.
(When every page was 10.0.5.2 and got every frame, each machine answered
the others' connections with a reset.)

    sudo tools/net-setup.sh        once per boot of this machine: the TAP device and the routing
    tools/net-relay.py             then this, as yourself

Only pages from the allowed origins may connect (by default the dev server on
this machine, port 8000). A WebSocket can be opened by any page in the
browser, and whoever connects here can send what it likes to this machine's
network: without the check, so could every web site that is open.

A relay that is open to the public is limited (docs/deploy.md, docker/net-allowlist.sh for the firewall side).
Everything a stranger can ask of it is rate-limited, so that no one can use it up (every limit is a flag here and an
RV_* variable in docker/relay.sh; 0 turns a limit off):
  requests    per client address (an IPv6 address counts as its /64): /address, /machines and WebSocket upgrades each
              have a token bucket, refused and failed ones count too; a bucket for all addresses together; the time
              to send the request (--handshake-secs) and the number of connections still doing so are limited. Over a
              limit: 429 with Retry-After.
  addresses   the held ones (/address) are limited per visitor, per network (/24, IPv6 /64) and in all; the oldest
              unused hold goes first, so the pool of 10.0.5.N cannot be used up by requests.
  machines    --max-clients in all; --strict makes each machine only what it was given: one hardware address, the
              IP address it asked for (frames that say another are dropped), IPv4 and ARP only; the traffic one
              machine can send (--rate-frames, --rate-bytes), its DNS queries (--dns-rate) and new TCP connections
              (--syn-rate) per second, what it can receive (--down-bytes), and everything one address uses, both ways
              (--ip-bytes); what a page can say to the relay besides frames (nothing today) is limited, one that says
              more is disconnected.
--trust-forwarded takes a client's address from the X-Forwarded-For header of the reverse proxy in front (Caddy).

--max-per-ip is how many machines from one public address have the network at once, and it is a share, not a wall
(on a campus or a public wifi many people have one address). A machine that comes when the address is at its limit is
not refused: it connects and WAITS (its IPv4 frames are dropped, ARP still works, so that it can be used at once when
its turn comes). The slots go to whoever needs them:
  - a slot that is free goes to the machine that has waited longest (a machine leaving frees one at once);
  - a waiting machine that sends traffic is "needy" (for NEED_WINDOW seconds after its last packet). When one is,
    and a machine of the same address that has the network has sent no unicast IPv4 packet for --idle-secs, the two
    change places: the idle one waits (at the end of the line), the needy one, the oldest of them, has the network.
    The idle one gets its place back the same way when it sends traffic again;
  - a machine that has just been given the network keeps it for --hold-secs, so that places do not change back and forth;
  - --max-waiting-per-ip bounds the line; beyond it a connection is refused (429).
A page that connects with &ctl=1 (dev.html does) is told where it stands by a TEXT message on its WebSocket (the Ethernet
frames are binary ones, so the two cannot be mistaken for each other); a page without ctl=1 is never sent text:
    {"type":"net-state","state":"active"|"waiting","others":N,"queue":Q,"waiting":W,"cap":C}
others: the other machines of this address that have the network now; queue: this machine's place in the line (0 if it
has the network); waiting: the length of the line; cap: --max-per-ip. It is sent when something in it changes.

No dependencies: the little of RFC 6455 that is needed is here.
"""
import argparse
import asyncio
import base64
import collections
import fcntl
import hashlib
import ipaddress
import json
import os
import struct
import time

TUNSETIFF = 0x400454CA
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 1, 2, 8, 9, 10
MAX_MESSAGE = 4096      # a frame is at most 1600 bytes: a longer message is not from a page of ours
TABLE_SIZE = 8192       # most keys any table of buckets keeps (the least recently used go first)
SWITCH_SIZE = 4096      # most hardware addresses the switch remembers (strict mode: one per machine)
MIN_ADDRESS, MAX_ADDRESS = 2, 250

clients = set()
state = {}      # a client's writer -> its Client record (limits, hardware address, address)
addresses = {}  # the last number of an address -> the client that has it, or the time it was handed out
handed = {}     # the last number of an address handed out and not used yet -> (when, visitor, visitor's network)
RESERVE_PER_IP = 4      # at most this many held for one visitor at a time (the oldest is handed out again; at least
                        # as many as a visitor's address may have machines, set in main())
PING_EVERY = 10         # seconds between the relay's pings; a page that has sent nothing (not even a pong) for
PING_DEAD = 35          # this long is gone (a closed laptop, a lost network): its address and slot are freed
NEED_WINDOW = 120       # seconds a waiting machine counts as needy after its last unicast IPv4 packet
queued_count = 0        # the place in the line of the machine that waits next
handshaking = 0         # connections that have not finished sending their request
switch = {}     # a hardware address (6 bytes) -> the client it was last seen from
ipbytes = {}    # a visitor's address -> the bucket of all the bytes its machines use, both ways


class Limits:
    max_clients = 0
    max_per_ip = 0          # machines of one address that have the network at once (0: no limit, nobody waits)
    max_waiting = 10        # machines of one address in the line, besides those
    idle = 60.0             # seconds without a packet before a machine's place can be given to a waiting one
    hold = 20.0             # seconds a machine keeps the network after it was given it
    rate_frames = 0         # per machine, per second (the bucket holds two seconds' worth)
    rate_bytes = 0
    dns_rate = 0            # per machine: DNS queries (UDP to port 53) per second
    syn_rate = 0            # per machine: new TCP connections per second
    down_bytes = 0          # per machine: bytes per second to it
    ip_bytes = 0            # per visitor address: bytes per second both ways, all its machines together
    strict = False
    trust_forwarded = False
    # (per minute, burst) per visitor address; the burst is what a page load needs and a little more
    address_per_min = 120   # (a lecture hall of people loading the page at once shares one address)
    machines_per_min = 30
    ws_per_min = 120
    burst = 40
    global_per_sec = 100    # requests of all visitors together (burst: three seconds' worth)
    handshake_secs = 5.0    # to send the request
    max_handshakes = 100    # connections that are still doing so
    address_hold = 60.0     # seconds an address that was handed out and not used is held for the page that asked
    holds_max = 100         # held addresses in all (the oldest unused goes first)
    holds_per_net = 24      # held addresses for one network (IPv4 /24, IPv6 /64)
    msg_rate = 20           # messages other than frames a page may send per second before it is disconnected


class Bucket:
    """A token bucket: `rate` tokens a second, at most `burst` kept."""
    __slots__ = ("rate", "burst", "tokens", "at")

    def __init__(self, rate, burst, now=None):
        self.rate, self.burst = rate, burst
        self.tokens = float(burst)
        self.at = time.monotonic() if now is None else now

    def take(self, n=1, now=None):
        now = time.monotonic() if now is None else now
        self.tokens = min(self.burst, self.tokens + (now - self.at) * self.rate)
        self.at = now
        if self.tokens >= n:
            self.tokens -= n
            return True
        return False

    def wait(self, n=1):
        """Seconds until n tokens are there."""
        return max(1, int((n - self.tokens) / self.rate) + 1) if self.rate else 1


class Buckets:
    """Buckets by key, for visitors: the table stays small (the least recently used key goes when it is full)."""

    def __init__(self, per_minute):
        self.per_minute = per_minute
        self.table = collections.OrderedDict()

    def take(self, key, now):
        """(True, 0), or (False, the seconds to wait)."""
        if not self.per_minute:
            return True, 0
        bucket = self.table.get(key)
        if bucket is None:
            bucket = self.table[key] = Bucket(self.per_minute / 60.0, Limits.burst, now)
            if len(self.table) > TABLE_SIZE:
                self.table.popitem(last=False)
        else:
            self.table.move_to_end(key)
        if bucket.take(1, now):
            return True, 0
        return False, bucket.wait()


request_limits = {"address": Buckets(0), "machines": Buckets(0), "ws": Buckets(0)}
global_requests = None


class Client:
    """What one connected page may do: its address, its one hardware address, and its share of the traffic."""

    def __init__(self, ip, number, now, writer=None, ctl=False):
        self.ip = ip              # where it connected from (the proxy's X-Forwarded-For, if trusted)
        self.number = number      # the last number of its 10.0.5.N, 0 if it did not say
        self.writer = writer
        self.ctl = ctl            # the page asked for control messages (&ctl=1)
        self.mac = None
        self.heard = now          # when the page last sent anything (frames, pongs)
        self.waiting = False      # in the line for the network (its IPv4 frames are dropped)
        self.queued = 0           # its place in the line, by the order in which machines began to wait
        self.since = now          # when it began to have the network (or to wait)
        self.last = now           # when it last sent a unicast IPv4 packet: what counts as using the network
        self.need = float("-inf")  # when it last sent one while it waited
        self.checked = 0.0        # when that last looked for a place for it
        self.told = None          # what it was last told (the control message)
        self.frames = float(Limits.rate_frames * 2)
        self.bytes = float(Limits.rate_bytes * 2)
        self.at = now
        self.dns = Bucket(Limits.dns_rate, Limits.dns_rate * 2, now) if Limits.dns_rate else None
        self.syn = Bucket(Limits.syn_rate, Limits.syn_rate * 2, now) if Limits.syn_rate else None
        self.down = Bucket(Limits.down_bytes, Limits.down_bytes * 2, now) if Limits.down_bytes else None
        self.msgs = Bucket(Limits.msg_rate, Limits.msg_rate * 2, now) if Limits.msg_rate else None

    def allow(self, size):
        """A token bucket for frames and for bytes (one second's worth, twice as a burst): False means drop."""
        if not (Limits.rate_frames or Limits.rate_bytes):
            return True
        now = time.monotonic()
        dt, self.at = now - self.at, now
        if Limits.rate_frames:
            self.frames = min(Limits.rate_frames * 2, self.frames + dt * Limits.rate_frames)
        if Limits.rate_bytes:
            self.bytes = min(Limits.rate_bytes * 2, self.bytes + dt * Limits.rate_bytes)
        if (Limits.rate_frames and self.frames < 1) or (Limits.rate_bytes and self.bytes < size):
            return False
        self.frames -= 1
        self.bytes -= size
        return True


def ipkey(raw):
    """A visitor's address as the limits count it: an IPv6 address is its /64 (one person has the whole of it)."""
    try:
        a = ipaddress.ip_address(raw)
    except ValueError:
        return raw
    if a.version == 6:
        if a.ipv4_mapped:
            return str(a.ipv4_mapped)
        return str(ipaddress.ip_network((a, 64), strict=False))
    return str(a)


def netkey(key):
    """The network a visitor's address is in, for the held addresses: an IPv4 /24, an IPv6 /64."""
    try:
        a = ipaddress.ip_address(key.split("/")[0])
    except ValueError:
        return key
    return str(ipaddress.ip_network((a, 24), strict=False)) if a.version == 4 else key


def transport_of(payload):
    """(protocol, offset of the transport header) of an IPv4 frame, or None if it is not a whole one."""
    if len(payload) < 34 or payload[14] >> 4 != 4:
        return None
    start = 14 + (payload[14] & 15) * 4
    # (the ports are in the first fragment only)
    return (payload[23], start) if start >= 34 and not struct.unpack(">H", payload[20:22])[0] & 0x1FFF else None


def frame_ok(client, others, payload):
    """--strict: is this frame what the machine is allowed to send? (Learns its hardware address from the first.)"""
    src = payload[6:12]
    if src[0] & 1:                                   # a group address as the source: never
        return False
    if client.mac is None:
        if any(c.mac == src for c in others):        # another machine's address: it would take its frames
            return False
        client.mac = src
    elif client.mac != src:
        return False
    ethertype = payload[12:14]
    mine = bytes([10, 0, 5, client.number])
    if ethertype == b"\x08\x06":                     # ARP: the sender must be this machine, in both addresses
        return len(payload) >= 42 and payload[22:28] == src and payload[28:32] == mine
    if ethertype == b"\x08\x00":                     # IPv4: from its own address
        return len(payload) >= 34 and payload[26:30] == mine
    return False                                     # IPv6 and the rest: this network is IPv4


def traffic_ok(client, payload, now):
    """The limits on what a machine that has the network does: DNS queries, new connections, the bytes of its
    visitor address. False: the frame is dropped, as on any network that is over its share."""
    if payload[12:14] == b"\x08\x00":
        found = transport_of(payload)
        if found:
            proto, t = found
            if client.dns and proto == 17 and payload[t + 2:t + 4] == b"\x00\x35" and not client.dns.take(1, now):
                return False
            if client.syn and proto == 6 and len(payload) >= t + 14 and payload[t + 13] & 0x12 == 0x02 \
                    and not client.syn.take(1, now):
                return False
    bucket = ipbytes.get(client.ip)
    return bucket is None or bucket.take(len(payload), now)


def sharing(ip):
    return [c for c in state.values() if c.ip == ip]


def idle_for(c, now):
    return now - max(c.last, c.since)


def say(c):
    """Tells the page where it stands, if it asked and if that changed."""
    if not (c.ctl and Limits.max_per_ip and c.writer):
        return
    mates = sharing(c.ip)
    line = sorted((m for m in mates if m.waiting), key=lambda m: m.queued)
    message = json.dumps({"type": "net-state", "state": "waiting" if c.waiting else "active",
                          "others": sum(1 for m in mates if m is not c and not m.waiting),
                          "queue": line.index(c) + 1 if c.waiting else 0, "waiting": len(line),
                          "cap": Limits.max_per_ip}, separators=(",", ":"))
    if message != c.told:
        c.told = message
        try:
            c.writer.write(encode(OP_TEXT, message.encode()))
        except Exception:
            pass


def rebalance(ip, now):
    """The machines of one address and the network: a free slot goes to the one that has waited longest; a needy one
    takes the place of one that has been idle (see the top of this file). Tells every page whose state changed."""
    global queued_count
    if not Limits.max_per_ip:
        return
    mates = sharing(ip)
    have = [c for c in mates if not c.waiting]
    line = sorted((c for c in mates if c.waiting), key=lambda c: c.queued)
    while line and len(have) < Limits.max_per_ip:
        c = line.pop(0)
        c.waiting, c.since = False, now
        have.append(c)
        print("network given to %s (10.0.5.%d), waited its turn" % (ip, c.number), flush=True)
    for c in [c for c in line if now - c.need < NEED_WINDOW]:
        idle = [h for h in have if idle_for(h, now) > Limits.idle and now - h.since > Limits.hold]
        if not idle:
            break
        h = max(idle, key=lambda h: idle_for(h, now))
        queued_count += 1
        print("network of %s: 10.0.5.%d (idle %ds) waits, 10.0.5.%d has it" % (ip, h.number, idle_for(h, now), c.number),
              flush=True)
        h.waiting, h.queued, h.since, h.need = True, queued_count, now, float("-inf")
        c.waiting, c.since, c.last = False, now, now
        have.remove(h)
        have.append(c)
    for c in mates:
        say(c)


def open_tap(name):
    fd = os.open("/dev/net/tun", os.O_RDWR | os.O_NONBLOCK)
    fcntl.ioctl(fd, TUNSETIFF, struct.pack("16sH", name.encode(), IFF_TAP | IFF_NO_PI))
    return fd


def encode(opcode, payload):
    """One unmasked frame, as a server sends them."""
    head = bytes([0x80 | opcode])
    if len(payload) < 126:
        head += bytes([len(payload)])
    elif len(payload) < 65536:
        head += bytes([126]) + struct.pack(">H", len(payload))
    else:
        head += bytes([127]) + struct.pack(">Q", len(payload))
    return head + payload


async def read_message(reader):
    """The next frame from a client: (opcode, payload). Clients mask what they send."""
    b0, b1 = await reader.readexactly(2)
    length = b1 & 0x7F
    if length == 126:
        (length,) = struct.unpack(">H", await reader.readexactly(2))
    elif length == 127:
        (length,) = struct.unpack(">Q", await reader.readexactly(8))
    if length > MAX_MESSAGE:    # a frame is at most 1600 bytes: this is not a page of ours
        raise ConnectionError("message too long")
    mask = await reader.readexactly(4) if b1 & 0x80 else None
    payload = await reader.readexactly(length)
    if mask:
        payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return b0 & 0x0F, payload


def client_ip(writer, headers):
    ip = writer.get_extra_info("peername", ("?",))[0]
    if Limits.trust_forwarded and headers.get("x-forwarded-for"):
        last = headers["x-forwarded-for"].split(",")[-1].strip()   # (the last: what the proxy itself saw)
        try:
            ipaddress.ip_address(last)
            ip = last
        except ValueError:
            pass                    # (not an address: the proxy's own)
    return ipkey(ip)


def free_address(ip, now):
    """The last number of an address nobody has and nobody holds, for the visitor `ip`; None if there is none.
    Held ones expire; one visitor holds a few at most, one network a few more, all together at most holds_max
    (in each case the oldest is handed out again), so that no number of requests uses the pool up."""
    taken = {c.number for c in state.values()}
    for n in [n for n, (t, _, _) in handed.items() if now - t > Limits.address_hold or n in taken]:
        del handed[n]
    net = netkey(ip)
    for who_is, limit in ((lambda h: h[1] == ip, RESERVE_PER_IP), (lambda h: h[2] == net, Limits.holds_per_net)):
        mine = sorted((h[0], n) for n, h in handed.items() if who_is(h))
        for _, n in mine[:max(0, len(mine) - limit + 1)]:
            del handed[n]
    while Limits.holds_max and len(handed) >= Limits.holds_max:
        del handed[min(handed, key=lambda n: handed[n][0])]
    number = next((n for n in range(MIN_ADDRESS, MAX_ADDRESS + 1) if n not in taken and n not in handed), None)
    if number is not None:
        handed[number] = (now, ip, net)
    return number


async def keepalive(writer, me):
    """Pings the page; one that has said nothing for PING_DEAD seconds is closed (its read then ends)."""
    while True:
        await asyncio.sleep(PING_EVERY)
        if time.monotonic() - me.heard > PING_DEAD:
            print("silent for %ds: closing %s" % (PING_DEAD, me.ip), flush=True)
            writer.close()
            return
        try:
            writer.write(encode(OP_PING, b""))
        except Exception:
            return


def reply(writer, status, origin=b"", more=b"", body=b"", allowed=False):
    cors = b"Access-Control-Allow-Origin: " + origin + b"\r\n" if origin and allowed else b""
    writer.write(b"HTTP/1.1 " + status + b"\r\n" + more + cors + b"Content-Length: " + str(len(body)).encode()
                 + b"\r\nConnection: close\r\n\r\n" + body)


async def serve(reader, writer, tap, origins):
    global handshaking, queued_count
    watch = None
    me = None
    try:
        if Limits.max_handshakes and handshaking >= Limits.max_handshakes:
            return                  # (too many are slow to say what they want: this one is not heard)
        handshaking += 1
        try:
            request = (await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), Limits.handshake_secs)).decode("latin-1")
        finally:
            handshaking -= 1
        headers = {}
        for line in request.split("\r\n")[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        origin = headers.get("origin", "")
        target = request.split("\r\n")[0].split(" ")[1] if " " in request.split("\r\n")[0] else "/"
        ip = client_ip(writer, headers)
        kind = "address" if target.startswith("/address") else "machines" if target.startswith("/machines") else "ws"
        # The limits on requests count all of them, the refused and failed ones too (a visitor who keeps trying
        # to connect is told to wait, whatever it was refused for).
        now = time.monotonic()
        ok, wait = (False, global_requests.wait()) if global_requests and not global_requests.take(1, now) else (True, 0)
        if ok:
            ok, wait = request_limits[kind].take(ip, now)
        if not ok:
            print("too many %s requests from %s: told to wait %ds" % (kind, ip, wait), flush=True)
            reply(writer, b"429 Too Many Requests", origin.encode(), b"Retry-After: %d\r\n" % wait, allowed=origin in origins)
            return
        # (A plain GET of /address or /machines may come with no Origin at all: a page asking its own origin, which
        # is where it is when the relay is behind a reverse proxy on the page's address, sends none. It reveals
        # nothing and reads no page's data; a WebSocket, which any page can open, must name an allowed origin.)
        plain = kind != "ws"
        if origin not in origins and not (plain and not origin):
            print("refused a connection from origin %r" % origin, flush=True)
            writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            return
        if kind == "address":
            # A free address, held for whoever asked: it connects with it soon, or never does.
            number = free_address(ip, now)
            if number is None:
                reply(writer, b"503 Service Unavailable", more=b"Retry-After: 10\r\n")
            else:
                reply(writer, b"200 OK", origin.encode(), b"Content-Type: text/plain\r\nCache-Control: no-store\r\n",
                      str(number).encode(), allowed=True)
            await writer.drain()
            return
        if kind == "machines":
            # The machines of the visitor that asks, by the last number of their address (tools/guest-port.py asks):
            # nothing of anyone else's.
            body = " ".join(str(n) for n, w in sorted(addresses.items()) if w in clients and state[w].ip == ip).encode()
            reply(writer, b"200 OK", more=b"Content-Type: text/plain\r\nCache-Control: no-store\r\n", body=body)
            await writer.drain()
            return
        if "sec-websocket-key" not in headers:
            writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            return
        number = int(target.split("ip=")[1].split("&")[0]) if "ip=" in target and target.split("ip=")[1].split("&")[0].isdigit() else 0
        refusal = None
        if Limits.max_clients and len(clients) >= Limits.max_clients:
            refusal = b"503 Service Unavailable"
        elif Limits.max_per_ip and len(sharing(ip)) >= Limits.max_per_ip + Limits.max_waiting:
            refusal = b"429 Too Many Requests"       # (up to --max-per-ip have the network, up to --max-waiting wait)
        elif Limits.strict and not MIN_ADDRESS <= number <= MAX_ADDRESS:
            refusal = b"400 Bad Request"
        elif Limits.strict and any(c.number == number for c in state.values()):
            refusal = b"409 Conflict"                       # another page has this address
        if refusal:
            print("refused %s (%s)" % (ip, refusal.decode()), flush=True)
            writer.write(b"HTTP/1.1 " + refusal + b"\r\nRetry-After: 30\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        ctl = "ctl=1" in target.partition("?")[2].split("&")
        me = state[writer] = Client(ip, number, now, writer, ctl)
        if Limits.ip_bytes and ip not in ipbytes:
            ipbytes[ip] = Bucket(Limits.ip_bytes, Limits.ip_bytes * 2, now)
        if Limits.max_per_ip:           # it waits until rebalance finds a place for it: at once if there is one
            queued_count += 1
            me.waiting, me.queued = True, queued_count
        if number:
            addresses[number] = writer
            handed.pop(number, None)
        accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + WS_GUID).digest())
        writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        clients.add(writer)
        watch = asyncio.create_task(keepalive(writer, me))
        print("connected: %s%s (%d now)" % (origin, ", 10.0.5.%d" % number if number else "", len(clients)), flush=True)
        rebalance(ip, time.monotonic())
        while True:
            opcode, payload = await read_message(reader)
            now = me.heard = time.monotonic()
            if opcode == OP_CLOSE:
                break
            frame = opcode == OP_BINARY and 14 <= len(payload) <= 1600
            if not frame and me.msgs and not me.msgs.take(1, now):
                # Anything but frames (the pings and pongs that keep the socket, a message the relay does not
                # know) comes at a few a second from a page of ours; more is not one.
                print("too many messages from %s: closing" % me.ip, flush=True)
                break
            if opcode == OP_PING:
                writer.write(encode(OP_PONG, payload))
            elif frame:
                if not me.allow(len(payload)):
                    continue            # over its share: a frame lost, as on any network
                if Limits.strict and not frame_ok(me, [c for w, c in state.items() if w is not writer], payload):
                    continue
                if Limits.max_per_ip:
                    if payload[12:14] == b"\x08\x00" and not payload[0] & 1:    # unicast IPv4: the network in use
                        me.last = now
                        if me.waiting:
                            me.need = now
                            if now - me.checked >= 0.5:
                                me.checked = now
                                rebalance(me.ip, now)       # (it may take the place of one that is idle)
                    if me.waiting and payload[12:14] != b"\x08\x06":
                        continue            # waits: ARP goes through (so that it is ready), the rest is dropped
                if not traffic_ok(me, payload, now):
                    continue
                if payload[6:12] not in switch and len(switch) >= SWITCH_SIZE:
                    del switch[next(iter(switch))]      # (a machine that makes up addresses cannot fill the table)
                switch[payload[6:12]] = writer
                try:
                    os.write(tap, payload)
                except (BlockingIOError, OSError):
                    pass    # the device is busy or down: a frame lost, as on any network
    except (asyncio.IncompleteReadError, asyncio.TimeoutError, asyncio.LimitOverrunError, ConnectionError, ValueError):
        pass
    finally:
        if watch:
            watch.cancel()
        state.pop(writer, None)
        if me is not None:
            rebalance(me.ip, time.monotonic())      # (its place goes to the one that has waited longest)
            if not sharing(me.ip):
                ipbytes.pop(me.ip, None)
        if writer in clients:
            clients.discard(writer)
            for table in (addresses, switch):
                for key in [k for k, v in table.items() if v is writer]:
                    del table[key]
            print("disconnected (%d left)" % len(clients), flush=True)
        writer.close()


def from_tap(tap):
    """A frame from the device goes to the client it is for; to all, if it is for all or for nobody known."""
    now = time.monotonic()
    while True:
        try:
            frame = os.read(tap, 2048)
        except (BlockingIOError, InterruptedError):
            return
        message = encode(OP_BINARY, frame)
        to = switch.get(frame[0:6]) if len(frame) >= 6 and not frame[0] & 1 else None
        for writer in ([to] if to in clients else list(clients)):
            mine = state.get(writer)
            if mine is not None:
                if mine.waiting and frame[12:14] != b"\x08\x06":
                    continue                # it waits for the network: ARP only
                if not mine.waiting:
                    bucket = ipbytes.get(mine.ip)
                    if (mine.down and not mine.down.take(len(frame), now)) or (bucket and not bucket.take(len(frame), now)):
                        continue            # over its share (or its visitor address's): a frame lost
            if writer.transport.get_write_buffer_size() < 1 << 20:  # else it is not keeping up: drop
                writer.write(message)


async def tick():
    """Once a second: a machine that began to wait may be able to take the place of one that has gone idle since."""
    while True:
        await asyncio.sleep(1)
        now = time.monotonic()
        for ip in {c.ip for c in list(state.values()) if c.waiting}:
            rebalance(ip, now)


def configure(args):
    global RESERVE_PER_IP, global_requests
    Limits.max_clients, Limits.max_per_ip, Limits.max_waiting = args.max_clients, args.max_per_ip, args.max_waiting_per_ip
    Limits.idle, Limits.hold = args.idle_secs, args.hold_secs
    Limits.rate_frames, Limits.rate_bytes = args.rate_frames, args.rate_bytes
    Limits.dns_rate, Limits.syn_rate, Limits.down_bytes, Limits.ip_bytes = args.dns_rate, args.syn_rate, args.down_bytes, args.ip_bytes
    Limits.strict, Limits.trust_forwarded = args.strict, args.trust_forwarded
    Limits.address_per_min, Limits.machines_per_min, Limits.ws_per_min = args.address_per_min, args.machines_per_min, args.ws_per_min
    Limits.burst, Limits.global_per_sec = args.burst, args.global_per_sec
    Limits.handshake_secs, Limits.max_handshakes = args.handshake_secs, args.max_handshakes
    Limits.address_hold, Limits.holds_max, Limits.holds_per_net = args.address_hold_secs, args.holds_max, args.holds_per_net
    Limits.msg_rate = args.msg_rate
    request_limits["address"], request_limits["machines"], request_limits["ws"] = (
        Buckets(Limits.address_per_min), Buckets(Limits.machines_per_min), Buckets(Limits.ws_per_min))
    global_requests = Bucket(Limits.global_per_sec, Limits.global_per_sec * 3) if Limits.global_per_sec else None
    RESERVE_PER_IP = max(4, Limits.max_per_ip + Limits.max_waiting)
    # (the held addresses and the machines must leave numbers to hand out)
    if Limits.holds_max:
        Limits.holds_max = max(1, min(Limits.holds_max, MAX_ADDRESS - MIN_ADDRESS + 1 - (Limits.max_clients or 40) - 10))


def parser():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--tap", default="rvtap0")
    p.add_argument("--port", type=int, default=8001)
    p.add_argument("--bind", default="127.0.0.1")
    p.add_argument("--origin", action="append",
                   help="an origin allowed to connect (default: http://localhost:8000 and http://127.0.0.1:8000)")
    p.add_argument("--max-clients", type=int, default=0, help="most machines at once (0: no limit)")
    p.add_argument("--max-per-ip", type=int, default=0,
                   help="most machines from one address that have the network at once; the others wait (0: no limit)")
    p.add_argument("--max-waiting-per-ip", type=int, default=Limits.max_waiting, help="most machines from one address that wait")
    p.add_argument("--idle-secs", type=float, default=Limits.idle,
                   help="a machine with no packet for this long gives its place to a waiting one that needs it")
    p.add_argument("--hold-secs", type=float, default=Limits.hold, help="a machine that was given the network keeps it this long")
    p.add_argument("--rate-frames", type=int, default=0, help="frames per second one machine may send (0: no limit)")
    p.add_argument("--rate-bytes", type=int, default=0, help="bytes per second one machine may send (0: no limit)")
    p.add_argument("--dns-rate", type=int, default=0, help="DNS queries per second one machine may send (0: no limit)")
    p.add_argument("--syn-rate", type=int, default=0, help="new TCP connections per second one machine may make (0: no limit)")
    p.add_argument("--down-bytes", type=int, default=0, help="bytes per second one machine may receive (0: no limit)")
    p.add_argument("--ip-bytes", type=int, default=0,
                   help="bytes per second all the machines of one address may use, both ways (0: no limit)")
    p.add_argument("--address-per-min", type=int, default=Limits.address_per_min, help="GET /address per minute per address (0: no limit)")
    p.add_argument("--machines-per-min", type=int, default=Limits.machines_per_min, help="GET /machines per minute per address")
    p.add_argument("--ws-per-min", type=int, default=Limits.ws_per_min, help="WebSocket upgrades per minute per address, refused ones too")
    p.add_argument("--burst", type=int, default=Limits.burst, help="requests an address may make at once, before the per-minute rate")
    p.add_argument("--global-per-sec", type=int, default=Limits.global_per_sec, help="requests per second of all addresses together")
    p.add_argument("--handshake-secs", type=float, default=Limits.handshake_secs, help="seconds a connection has to send its request")
    p.add_argument("--max-handshakes", type=int, default=Limits.max_handshakes, help="connections that may be sending their request at once")
    p.add_argument("--address-hold-secs", type=float, default=Limits.address_hold, help="seconds an unused address is held for the page that asked")
    p.add_argument("--holds-max", type=int, default=Limits.holds_max, help="held addresses in all")
    p.add_argument("--holds-per-net", type=int, default=Limits.holds_per_net, help="held addresses for one network (IPv4 /24, IPv6 /64)")
    p.add_argument("--msg-rate", type=int, default=Limits.msg_rate, help="messages other than frames a page may send per second")
    p.add_argument("--strict", action="store_true",
                   help="a machine has one hardware address and the IP address it asked for; IPv4 and ARP only")
    p.add_argument("--trust-forwarded", action="store_true",
                   help="the client's address is the last entry of X-Forwarded-For (a reverse proxy in front)")
    return p


async def main():
    args = parser().parse_args()
    configure(args)
    origins = set(args.origin or ["http://localhost:8000", "http://127.0.0.1:8000"])

    tap = open_tap(args.tap)
    asyncio.get_running_loop().add_reader(tap, from_tap, tap)
    server = await asyncio.start_server(lambda r, w: serve(r, w, tap, origins), args.bind, args.port, limit=1 << 14)
    print("relaying %s on ws://%s:%d for %s" % (args.tap, args.bind, args.port, ", ".join(sorted(origins))), flush=True)
    asyncio.create_task(tick())
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
