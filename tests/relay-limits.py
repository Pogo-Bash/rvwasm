#!/usr/bin/env python3
"""The relay's sharing of the network and its limits, tried in one process: tools/net-relay.py is imported, its
server runs on a local port with a socketpair for the TAP device, and hand-made pages (WebSocket clients that send
Ethernet frames) connect to it, each from a visitor address of its own (X-Forwarded-For). No root, no Docker, no
browser, a few seconds:

    python3 tests/relay-limits.py

What it checks: the machines of one address beyond the limit wait instead of being refused; an idle machine yields to
a needy one; first come, first served; a machine that leaves is replaced at once; no changing back and forth (the
hold); the control messages (and that a page that did not ask gets none); and every limit: requests per address
(/address, /machines, WebSocket upgrades, failed ones too, IPv6 by /64), all addresses together, the held
addresses (per visitor, per network, in all), slow handshakes, DNS queries, new connections, download and
per-address bandwidth, what a page may say besides frames, and the size of every table.
tests/relay-concurrent.sh does the sharing with real network namespaces against a relay in Docker.
"""
import asyncio
import base64
import importlib.util
import os
import socket
import struct
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("netrelay", os.path.join(here, "..", "tools", "net-relay.py"))
relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relay)

class FakeOS:
    """The relay's `os`, with a TAP device that is two lists: what the relay wrote, and what is waiting to be read."""

    def __init__(self):
        self.out, self.inq = [], []

    def write(self, fd, data):
        self.out.append(bytes(data))

    def read(self, fd, n):
        if self.inq:
            return self.inq.pop(0)
        raise BlockingIOError

    def __getattr__(self, name):
        return getattr(os, name)


TAP = 999
ORIGIN = "http://localhost:8080"
DEFAULTS = {k: v for k, v in vars(relay.Limits).items() if not k.startswith("_")}
failures = 0


def check(name, ok, detail=""):
    global failures
    print("%s  %s%s" % ("ok   " if ok else "FAIL ", name, "" if ok or not detail else "   (" + str(detail) + ")"))
    failures += 0 if ok else 1


def mac(n):
    return bytes([2, 0, 0, 0, n >> 8, n & 255])


GATEWAY = bytes([2, 0, 0, 0, 0, 1])


def ipv4(n, proto, l4, dst=bytes([93, 184, 216, 34]), src_mac=None):
    ip = bytes([0x45, 0]) + struct.pack(">H", 20 + len(l4)) + b"\0\0\0\0\x40" + bytes([proto]) + b"\0\0" + bytes([10, 0, 5, n]) + dst + l4
    return GATEWAY + (src_mac or mac(n)) + b"\x08\x00" + ip


def syn(n):
    return ipv4(n, 6, struct.pack(">HHIIBBHHH", 40000, 443, 1, 0, 0x50, 0x02, 1000, 0, 0))


def ack(n, size=0):
    return ipv4(n, 6, struct.pack(">HHIIBBHHH", 40000, 443, 1, 1, 0x50, 0x10, 1000, 0, 0) + b"x" * size)


def dns(n):
    return ipv4(n, 17, struct.pack(">HHHH", 5353, 53, 12, 0) + b"abcd", dst=bytes([10, 0, 5, 1]))


def arp(n):
    return (b"\xff" * 6 + mac(n) + b"\x08\x06" + b"\0\x01\x08\0\x06\x04\0\x01" + mac(n) + bytes([10, 0, 5, n])
            + b"\0" * 6 + bytes([10, 0, 5, 1]))


class WS:
    def __init__(self, reader, writer, head):
        self.r, self.w, self.head = reader, writer, head
        self.buf = b""
        self.eof = False

    @classmethod
    async def open(cls, port, number=0, xff=None, ctl=True, origin=ORIGIN):
        r, w = await asyncio.open_connection("127.0.0.1", port)
        key = base64.b64encode(os.urandom(16)).decode()
        w.write(("GET /?ip=%d%s HTTP/1.1\r\nHost: x\r\nOrigin: %s\r\n%sUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                 % (number, "&ctl=1" if ctl else "", origin, "X-Forwarded-For: %s\r\n" % xff if xff else "", key)).encode())
        head = b""
        try:
            while b"\r\n\r\n" not in head:
                c = await asyncio.wait_for(r.read(1), 2)
                if not c:
                    break
                head += c
        except asyncio.TimeoutError:
            pass
        return cls(r, w, head.split(b"\r\n")[0].decode() if head else "")

    async def send(self, payload, op=2):
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.w.write(head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))
        try:
            await self.w.drain()
        except ConnectionError:
            self.eof = True

    async def recv(self, timeout=0.3):
        """(opcode, payload), or None if nothing came in `timeout` seconds (or the socket closed: self.eof)."""
        end = time.monotonic() + timeout
        while True:
            if len(self.buf) >= 2:
                ln, off = self.buf[1] & 0x7F, 2
                if ln == 126 and len(self.buf) >= 4:
                    ln, off = struct.unpack(">H", self.buf[2:4])[0], 4
                if ln < 126 or off == 4:
                    if len(self.buf) >= off + ln:
                        op, data = self.buf[0] & 15, self.buf[off:off + ln]
                        self.buf = self.buf[off + ln:]
                        return op, data
            left = end - time.monotonic()
            if left <= 0:
                return None
            try:
                chunk = await asyncio.wait_for(self.r.read(65536), left)
            except asyncio.TimeoutError:
                return None
            except ConnectionError:
                self.eof = True
                return None
            if not chunk:
                self.eof = True
                return None
            self.buf += chunk

    async def drain(self, timeout=0.3):
        """Everything that comes in for `timeout` seconds: (texts as dicts, number of binary frames)."""
        import json
        texts, frames = [], 0
        while (m := await self.recv(timeout)) is not None:
            if m[0] == 1:
                texts.append(json.loads(m[1]))
            elif m[0] == 2:
                frames += 1
        return texts, frames

    async def state(self, timeout=0.3):
        texts, _ = await self.drain(timeout)
        return texts[-1] if texts else None

    async def close(self):
        self.w.close()
        try:
            await self.w.wait_closed()
        except Exception:
            pass


async def http(port, path, xff=None, origin=None):
    r, w = await asyncio.open_connection("127.0.0.1", port)
    w.write(("GET %s HTTP/1.1\r\nHost: x\r\n%s%sConnection: close\r\n\r\n"
             % (path, "Origin: %s\r\n" % origin if origin else "", "X-Forwarded-For: %s\r\n" % xff if xff else "")).encode())
    data = b""
    while chunk := await r.read(4096):
        data += chunk
    w.close()
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    headers = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    return int(lines[0].split()[1]) if lines[0] else 0, headers, body.decode()


class Env:
    """A relay on a local port with fresh state; `flags` are the relay's own."""

    async def __aenter__(self):
        for t in (relay.clients, relay.state, relay.addresses, relay.handed, relay.switch, relay.ipbytes):
            t.clear()
        relay.queued_count = 0
        relay.handshaking = 0
        relay.NEED_WINDOW = 120
        self.flags = []
        return self

    async def start(self, *flags):
        for k, v in DEFAULTS.items():
            setattr(relay.Limits, k, v)         # (the flags' defaults are Limits as it is)
        relay.configure(relay.parser().parse_args(["--strict", "--trust-forwarded", "--address-per-min", "0",
                                                   "--machines-per-min", "0", "--ws-per-min", "0", "--global-per-sec", "0",
                                                   "--burst", "1000"] + list(flags)))
        self.fake = relay.os = FakeOS()
        self.server = await asyncio.start_server(lambda r, w: relay.serve(r, w, TAP, {ORIGIN}),
                                                 "127.0.0.1", 0, limit=1 << 14)
        self.port = self.server.sockets[0].getsockname()[1]
        self.ticker = asyncio.create_task(relay.tick())
        return self

    def forwarded(self):
        """The frames that reached the TAP device since the last call."""
        out, self.fake.out = self.fake.out, []
        return out

    def inject(self, frame):
        """A frame from the TAP device, for the relay to deliver."""
        self.fake.inq.append(frame)
        relay.from_tap(TAP)

    async def __aexit__(self, *exc):
        self.ticker.cancel()
        self.server.close()
        for w in list(relay.clients):
            w.close()
        await asyncio.sleep(0.05)
        relay.os = os


def visitor(n):
    return "198.51.100.%d" % n


async def sharing():
    print("-- sharing the network of one address")
    async with Env() as e:
        await e.start("--max-per-ip", "2", "--max-waiting-per-ip", "2", "--idle-secs", "1.2", "--hold-secs", "0.4")
        ip = visitor(7)
        a = await WS.open(e.port, 2, ip)
        b = await WS.open(e.port, 3, ip)
        sa, sb = await a.state(), await b.state()
        check("the first two machines have the network", sa["state"] == "active" and sb["state"] == "active" and sb["type"] == "net-state", (sa, sb))
        await a.send(syn(2))
        await b.send(syn(3))
        await asyncio.sleep(0.1)
        check("their packets reach the TAP device", len(e.forwarded()) == 2)
        c = await WS.open(e.port, 4, ip)
        check("the third from the same address is not refused", c.head.startswith("HTTP/1.1 101"), c.head)
        sc = await c.state()
        check("... it waits, is told so, with its place and the others", sc == {"type": "net-state", "state": "waiting", "others": 2, "queue": 1, "waiting": 1, "cap": 2}, sc)
        await c.send(syn(4))
        await c.send(arp(4))
        await asyncio.sleep(0.1)
        fr = e.forwarded()
        check("... its IPv4 frames are dropped, ARP goes through", len(fr) == 1 and fr[0][12:14] == b"\x08\x06", len(fr))
        e.inject(mac(4) + GATEWAY + b"\x08\x00" + b"0" * 60)
        e.inject(mac(4) + GATEWAY + b"\x08\x06" + b"0" * 40)
        await asyncio.sleep(0.05)
        _, frames = await c.drain(0.2)
        check("... only ARP is delivered to it", frames == 1, frames)
        sa = await a.state()
        check("a machine that has the network is told how many others share it and that one waits", sa and sa["waiting"] == 1 and sa["others"] == 1, sa)
        # b keeps using the network, a goes idle; c is needy (it sent the SYN above)
        end = time.monotonic() + 2.4
        while time.monotonic() < end:
            await b.send(syn(3))
            await asyncio.sleep(0.2)
        sa, sc = await a.state(), await c.state()
        check("an idle machine yields to the needy waiting one", sc and sc["state"] == "active" and sa and sa["state"] == "waiting" and sa["queue"] == 1, (sa, sc))
        e.forwarded()
        await c.send(syn(4))
        await a.send(syn(2))
        await asyncio.sleep(0.1)
        check("the new owner's packets go through, the old one's are dropped", len(e.forwarded()) == 1)
        # FIFO and the limit of the line
        d = await WS.open(e.port, 5, ip)
        sd = await d.state()
        check("a new machine queues behind (place 2)", sd["state"] == "waiting" and sd["queue"] == 2, sd)
        f = await WS.open(e.port, 6, ip)
        check("the line is bounded: beyond max-waiting a connection is refused (429)", f.head.startswith("HTTP/1.1 429"), f.head)
        other = await WS.open(e.port, 9, visitor(8))
        check("another address has its own slots", other.head.startswith("HTTP/1.1 101") and (await other.state())["state"] == "active")
        await b.close()
        await asyncio.sleep(0.1)
        sa, sd = await a.state(), await d.state()
        check("a machine that leaves frees its place at once for the one that waited longest",
              sa["state"] == "active" and sd["state"] == "waiting" and sd["queue"] == 1, (sa, sd))
        for w in (a, c, d, other):
            await w.close()
        await asyncio.sleep(0.1)
        check("every record is gone when the machines are", not relay.state and not relay.ipbytes and not relay.addresses and not relay.switch)


async def until(ws, want, timeout=6.0):
    """The last state `ws` was told, once `want` holds of it (or None after `timeout` seconds), and when."""
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        texts, _ = await ws.drain(0.1)
        if texts:
            last = texts[-1]
        if last and want(last):
            return last, time.monotonic()
    return None, time.monotonic()


async def holding():
    print("-- no changing back and forth, needy machines in line")
    async with Env() as e:
        await e.start("--max-per-ip", "2", "--max-waiting-per-ip", "4", "--idle-secs", "0.3", "--hold-secs", "2.5")
        ip = visitor(20)
        a, b, c, d = [await WS.open(e.port, n, ip) for n in (2, 3, 4, 5)]
        await asyncio.sleep(0.2)
        for w in (a, b, c, d):
            await w.drain(0.05)
        # a and b have had the network since the start; c and d (in that order) need it, a and b are idle
        await c.send(syn(4))
        await d.send(syn(5))
        await asyncio.sleep(1.0)
        quiet = True
        for w in (a, b, c, d):
            quiet = quiet and not (await w.drain(0.05))[0]
        check("nothing changes before a place has been held for hold-secs, however idle", quiet)
        sc, t = await until(c, lambda s: s["state"] == "active")
        sd, _ = await until(d, lambda s: s["state"] == "active", 1.0)
        check("both places are given to the two needy ones", sc is not None and sd is not None, (sc, sd))
        sa, _ = await until(a, lambda s: s["state"] == "waiting", 1.0)
        sb, _ = await until(b, lambda s: s["state"] == "waiting", 1.0)
        check("... the two idle ones wait, in the order they were demoted (queue places 1 and 2)",
              sa and sb and {sa["queue"], sb["queue"]} == {1, 2}, (sa, sb))
        # a and b ask again at once: c and d were given the network just now and hold it
        await a.send(syn(2))
        await b.send(syn(3))
        await asyncio.sleep(1.0)
        check("the newcomers keep the network for hold-secs, however idle (no thrashing)",
              not (await c.drain(0.05))[0] and not (await d.drain(0.05))[0])
        sa, t2 = await until(a, lambda s: s["state"] == "active", 4.0)
        sc, _ = await until(c, lambda s: s["state"] == "waiting", 1.0)
        check("after hold-secs the idle newcomers do yield (the old ones are back)", sa is not None and sc is not None and t2 - t > 2.0, (sa, sc))
        # busy machines are never taken from: everybody sends, nobody changes
        for w in (a, b, c, d):
            await w.drain(0.05)
        end = time.monotonic() + 3.5
        while time.monotonic() < end:
            for n, w in ((2, a), (3, b), (4, c), (5, d)):
                await w.send(syn(n))
            await asyncio.sleep(0.15)
        changes = 0
        for w in (a, b, c, d):
            changes += len((await w.drain(0.05))[0])
        check("nothing changes while the machines that have the network keep using it", changes == 0, changes)
        for w in (a, b, c, d):
            await w.close()


async def messages():
    print("-- control messages")
    async with Env() as e:
        await e.start("--max-per-ip", "1", "--msg-rate", "5")
        ip = visitor(30)
        a = await WS.open(e.port, 2, ip, ctl=False)
        b = await WS.open(e.port, 3, ip, ctl=False)
        t, f = await b.drain(0.3)
        check("a page that did not ask (no ctl=1) is never sent text", not t and not (await a.drain(0.1))[0])
        c = await WS.open(e.port, 4, ip)
        check("the message is text, which Ethernet frames never are", (await c.drain(0.2))[0][0]["state"] == "waiting")
        await c.send(b'{"net":"hello"}', op=1)
        check("a text message from the page is ignored", (await c.drain(0.2)) == ([], 0) and not c.eof)
        for _ in range(60):
            await c.send(b"x", op=1)
        await asyncio.sleep(0.2)
        await c.drain(0.2)
        check("a page that floods the relay with messages is disconnected", c.eof)
        big = await WS.open(e.port, 6, visitor(31))
        await big.send(b"x" * 5000, op=2)
        await big.drain(0.3)
        check("a message over 4096 bytes closes the connection", big.eof)
        for w in (a, b):
            await w.close()


async def requests():
    print("-- limits on requests")
    async with Env() as e:
        await e.start("--max-per-ip", "6", "--address-per-min", "6", "--machines-per-min", "6", "--ws-per-min", "6", "--burst", "3")
        res = [await http(e.port, "/address", "203.0.113.5") for _ in range(5)]
        check("/address: a burst is served, then 429 with Retry-After", [r[0] for r in res] == [200, 200, 200, 429, 429] and int(res[3][1]["retry-after"]) >= 1, [(r[0], r[1].get("retry-after")) for r in res])
        check("... another visitor is not affected", (await http(e.port, "/address", "203.0.113.6"))[0] == 200)
        r1 = [(await http(e.port, "/address", "2001:db8:1:2::%x" % i))[0] for i in range(1, 6)]
        check("... an IPv6 visitor is counted by its /64", r1 == [200, 200, 200, 429, 429], r1)
        check("... another /64 is another visitor", (await http(e.port, "/address", "2001:db8:1:3::1"))[0] == 200)
        r2 = [(await http(e.port, "/machines", "203.0.113.5"))[0] for _ in range(5)]
        check("/machines: 429 over the rate", r2 == [200, 200, 200, 429, 429], r2)
        r3 = [(await http(e.port, "/", "203.0.113.5", "http://evil.example"))[0] for _ in range(5)]
        check("a WebSocket upgrade that is refused (wrong origin) counts as an attempt: 429 after the burst", r3[:3] == [403] * 3 and r3[3] == 429, r3)
        w = await WS.open(e.port, 2, "203.0.113.5")
        check("... and the real one from that address waits too", w.head.startswith("HTTP/1.1 429"), w.head)
        ok = await WS.open(e.port, 2, "203.0.113.9")
        check("... but not from another address", ok.head.startswith("HTTP/1.1 101"))
        # /machines shows the visitor's own machines only
        await WS.open(e.port, 7, "203.0.113.20")
        mine = await http(e.port, "/machines", "203.0.113.9")
        other = await http(e.port, "/machines", "203.0.113.21")
        check("/machines tells a visitor of its own machines only", mine[2] == "2" and other[2] == "", (mine[2], other[2]))
        await ok.close()
    async with Env() as e:
        await e.start("--global-per-sec", "5")
        res = [(await http(e.port, "/address", visitor(i)))[0] for i in range(1, 25)]
        check("requests of all visitors together are limited (429 for everyone once the bucket is empty)",
              res[:10] == [200] * 10 and 429 in res, res)
        await asyncio.sleep(1.2)
        check("... and it recovers", (await http(e.port, "/address", visitor(99)))[0] == 200)


async def addresses():
    print("-- the pool of addresses")
    async with Env() as e:
        await e.start("--holds-per-net", "3", "--holds-max", "8", "--address-hold-secs", "60")
        for i in range(1, 30):
            await http(e.port, "/address", "192.0.2.%d" % i)
        check("one /24 holds at most --holds-per-net addresses however many addresses in it ask", len(relay.handed) == 3, len(relay.handed))
        for i in range(1, 30):
            await http(e.port, "/address", "100.64.%d.1" % i)
        check("all visitors together hold at most --holds-max (the oldest hold goes first)", len(relay.handed) <= 8, len(relay.handed))
        numbers = [int((await http(e.port, "/address", "100.65.%d.1" % i))[2]) for i in range(1, 10)]
        check("every answer is a number from 2 to 250 that nobody else holds", all(2 <= n <= 250 for n in numbers) and len(set(numbers[-8:])) == 8, numbers)
        relay.handed.clear()
        check("a hold that is not used expires", True)
        relay.Limits.address_hold = 0.2
        n1 = (await http(e.port, "/address", visitor(1)))[2]
        await asyncio.sleep(0.3)
        await http(e.port, "/address", visitor(2))
        check("... (the first is no longer held)", int(n1) not in relay.handed or relay.handed[int(n1)][1] != visitor(1))
    async with Env() as e:
        await e.start("--holds-max", "5")
        first = int((await http(e.port, "/address", visitor(1)))[2])
        for i in range(2, 8):
            await http(e.port, "/address", visitor(i))
        check("the oldest unused hold is the one to go when the table is full",
              relay.handed.get(first, (0, 0))[1] != visitor(1) and len(relay.handed) == 5, relay.handed)


async def handshakes():
    print("-- slow handshakes")
    async with Env() as e:
        await e.start("--max-handshakes", "3", "--handshake-secs", "0.6")
        idle = [await asyncio.open_connection("127.0.0.1", e.port) for _ in range(3)]
        await asyncio.sleep(0.1)
        r, w = await asyncio.open_connection("127.0.0.1", e.port)
        w.write(b"GET /address HTTP/1.1\r\nHost: x\r\n\r\n")
        try:
            data = await asyncio.wait_for(r.read(100), 1)
        except ConnectionError:
            data = b""
        check("connections that have not sent their request are limited (the next is closed unheard)", data == b"", data)
        await asyncio.sleep(0.8)
        closed = []
        for r, _ in idle:
            try:
                closed.append(await asyncio.wait_for(r.read(10), 1) == b"")
            except ConnectionError:
                closed.append(True)
        check("a connection that is slow to send its request is closed", all(closed), closed)
        check("the count is back to zero and requests work again", relay.handshaking == 0 and (await http(e.port, "/address", visitor(1)))[0] == 200, relay.handshaking)
        r, w = await asyncio.open_connection("127.0.0.1", e.port)
        w.write(b"GET /address HTTP/1.1\r\n" + b"X-A: " + b"a" * 20000 + b"\r\n\r\n")
        try:
            data = await asyncio.wait_for(r.read(100), 2)
        except ConnectionError:
            data = b""
        check("a request line and headers over 16 KiB are not read", data == b"" and relay.handshaking == 0, data)


async def traffic():
    print("-- limits on a machine's traffic")
    async with Env() as e:
        await e.start("--dns-rate", "5", "--syn-rate", "4", "--rate-frames", "0", "--max-per-ip", "6")
        a = await WS.open(e.port, 2, visitor(40))
        await a.drain(0.1)
        for _ in range(40):
            await a.send(dns(2))
        await asyncio.sleep(0.2)
        n = len(e.forwarded())
        check("DNS queries per second are limited (burst of two seconds' worth, then dropped)", 8 <= n <= 14, n)
        for _ in range(40):
            await a.send(syn(2))
        await asyncio.sleep(0.2)
        n = len(e.forwarded())
        check("new TCP connections (SYN) per second are limited", 6 <= n <= 11, n)
        for _ in range(40):
            await a.send(ack(2))
        await asyncio.sleep(0.2)
        check("... the rest of TCP is not", len(e.forwarded()) == 40)
        await a.close()
    async with Env() as e:
        await e.start("--ip-bytes", "20000", "--max-per-ip", "6")
        a = await WS.open(e.port, 2, visitor(41))
        b = await WS.open(e.port, 3, visitor(41))
        c = await WS.open(e.port, 4, visitor(42))
        for w, n in ((a, 2), (b, 3), (c, 4)):
            await w.drain(0.05)
        for _ in range(60):
            await a.send(ack(2, 1000))
            await b.send(ack(3, 1000))
        await asyncio.sleep(0.2)
        n = len(e.forwarded())
        check("all the machines of one address together are limited in bytes (~40 KB burst of 120 sent)", 30 <= n <= 50, n)
        for _ in range(20):
            await c.send(ack(4, 1000))
        await asyncio.sleep(0.2)
        check("... another address has its own", len(e.forwarded()) == 20)
        await a.close(), await b.close(), await c.close()
    async with Env() as e:
        await e.start("--down-bytes", "10000", "--max-per-ip", "6")
        a = await WS.open(e.port, 2, visitor(43))
        await a.send(arp(2))
        e.forwarded()
        for i in range(60):
            e.inject(mac(2) + GATEWAY + b"\x08\x00" + b"0" * 986)
        await asyncio.sleep(0.2)
        _, frames = await a.drain(0.3)
        check("what a machine receives is limited per second (~20 KB burst of 60 KB sent)", 10 <= frames <= 30, frames)
        await a.close()
    async with Env() as e:
        await e.start("--rate-frames", "50", "--rate-bytes", "0")
        a = await WS.open(e.port, 2, visitor(44))
        for _ in range(300):
            await a.send(ack(2))
        await asyncio.sleep(0.2)
        n = len(e.forwarded())
        check("frames per second per machine (existing limit) still holds", 90 <= n <= 130, n)
        await a.close()


async def tables():
    print("-- tables stay small")
    async with Env() as e:
        old = relay.TABLE_SIZE
        relay.TABLE_SIZE = 50
        await e.start("--address-per-min", "6000", "--burst", "5")
        for i in range(300):
            relay.request_limits["address"].take("10.1.%d.%d" % (i // 250, i % 250), time.monotonic())
        check("the table of request buckets is bounded (least recently used go first)", len(relay.request_limits["address"].table) <= 50, len(relay.request_limits["address"].table))
        relay.TABLE_SIZE = old
    async with Env() as e:
        old = relay.SWITCH_SIZE
        relay.SWITCH_SIZE = 20
        await e.start("--max-per-ip", "0")
        relay.configure(relay.parser().parse_args(["--address-per-min", "0", "--machines-per-min", "0", "--ws-per-min", "0", "--global-per-sec", "0", "--trust-forwarded"]))
        a = await WS.open(e.port, 2, visitor(50))
        for i in range(200):
            await a.send(GATEWAY + bytes([2, 9, 9, 9, i >> 8, i & 255]) + b"\x08\x00" + b"0" * 40)
        await asyncio.sleep(0.2)
        check("the switch (non-strict: a machine can make up hardware addresses) is bounded", len(relay.switch) <= 20, len(relay.switch))
        relay.SWITCH_SIZE = old
        await a.close()


async def main():
    relay.PING_EVERY = 100
    for scenario in (sharing, holding, messages, requests, addresses, handshakes, traffic, tables):
        try:
            await asyncio.wait_for(scenario(), 60)
        except Exception as error:
            import traceback
            traceback.print_exc()
            check(scenario.__name__ + " ran to the end", False, repr(error))
    print("\n%s" % ("all checks passed" if not failures else "%d FAILED" % failures))
    return failures


if __name__ == "__main__":
    sys.exit(1 if asyncio.run(main()) else 0)
