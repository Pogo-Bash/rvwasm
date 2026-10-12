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

A relay that is open to the public is limited (docs/deploy.md, docker/net-allowlist.sh for the firewall side):
--max-clients and --max-per-ip bound the machines, --rate-frames and --rate-bytes the traffic one machine can send,
and --strict makes each machine only what it was given: one hardware address, the IP address it asked for (frames
that say another are dropped), IPv4 and ARP only. --trust-forwarded takes a client's address from the
X-Forwarded-For header of the reverse proxy in front (Caddy), for the per-address limit.

No dependencies: the little of RFC 6455 that is needed is here.
"""
import argparse
import asyncio
import base64
import fcntl
import hashlib
import os
import struct
import time

TUNSETIFF = 0x400454CA
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 2, 8, 9, 10

clients = set()
state = {}      # a client's writer -> its Client record (limits, hardware address, address)
addresses = {}  # the last number of an address -> the client that has it, or the time it was handed out
handed = {}     # the last number of an address handed out and not used yet -> the visitor's address it went to
RESERVE = 300           # seconds a handed-out address is held for the page that asked
RESERVE_PER_IP = 4      # at most this many held for one visitor at a time (the oldest is handed out again)
PING_EVERY = 10         # seconds between the relay's pings; a page that has sent nothing (not even a pong) for
PING_DEAD = 35          # this long is gone (a closed laptop, a lost network): its address and slot are freed
switch = {}     # a hardware address (6 bytes) -> the client it was last seen from


class Limits:
    max_clients = 0
    max_per_ip = 0
    rate_frames = 0
    rate_bytes = 0
    strict = False
    trust_forwarded = False


class Client:
    """What one connected page may do: its address, its one hardware address, and its share of the traffic."""

    def __init__(self, ip, number, now):
        self.ip = ip              # where it connected from (the proxy's X-Forwarded-For, if trusted)
        self.number = number      # the last number of its 10.0.5.N, 0 if it did not say
        self.mac = None
        self.heard = now          # when the page last sent anything (frames, pongs)
        self.frames = float(Limits.rate_frames * 2)
        self.bytes = float(Limits.rate_bytes * 2)
        self.at = now

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
    if length > 65536:          # a frame is at most 1600 bytes: this is not a page of ours
        raise ConnectionError("message too long")
    mask = await reader.readexactly(4) if b1 & 0x80 else None
    payload = await reader.readexactly(length)
    if mask:
        payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return b0 & 0x0F, payload


def client_ip(writer, headers):
    ip = writer.get_extra_info("peername", ("?",))[0]
    if Limits.trust_forwarded and headers.get("x-forwarded-for"):
        ip = headers["x-forwarded-for"].split(",")[-1].strip()   # (the last: what the proxy itself saw)
    return ip


def free_address(ip, now):
    """The last number of an address nobody has and nobody holds, for the visitor `ip`; None if there is none.
    Held ones expire; one visitor holds a few at most (when it has that many, its oldest is handed out again)."""
    taken = {c.number for c in state.values()}
    for n in [n for n, (t, _) in handed.items() if now - t > RESERVE or n in taken]:
        del handed[n]
    mine = sorted((t, n) for n, (t, who) in handed.items() if who == ip)
    if len(mine) >= RESERVE_PER_IP:
        del handed[mine[0][1]]
    number = next((n for n in range(2, 251) if n not in taken and n not in handed), None)
    if number is not None:
        handed[number] = (now, ip)
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


async def serve(reader, writer, tap, origins):
    watch = None
    try:
        request = (await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)).decode("latin-1")
        headers = {}
        for line in request.split("\r\n")[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        origin = headers.get("origin", "")
        target = request.split("\r\n")[0].split(" ")[1] if " " in request.split("\r\n")[0] else "/"
        # (A plain GET of /address or /machines may come with no Origin at all: a page asking its own origin, which
        # is where it is when the relay is behind a reverse proxy on the page's address, sends none. It reveals
        # nothing and reads no page's data; a WebSocket, which any page can open, must name an allowed origin.)
        plain = target.startswith(("/address", "/machines"))
        if origin not in origins and not (plain and not origin):
            print("refused a connection from origin %r" % origin, flush=True)
            writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            return
        if target.startswith("/address"):
            # A free address, held for whoever asked: it connects with it soon, or never does.
            number = free_address(client_ip(writer, headers), time.monotonic())
            if number is None:
                writer.write(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                await writer.drain()
                return
            body = str(number).encode()
            cors = b"Access-Control-Allow-Origin: " + origin.encode() + b"\r\n" if origin else b""
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\n" + cors
                         + b"Content-Length: " + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
            await writer.drain()
            return
        if target.startswith("/machines"):
            # The machines connected now, by the last number of their address (tools/guest-port.py asks).
            body = " ".join(str(n) for n, v in sorted(addresses.items()) if v in clients).encode()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\nContent-Length: "
                         + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
            await writer.drain()
            return
        if "sec-websocket-key" not in headers:
            writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            return
        number = int(target.split("ip=")[1].split("&")[0]) if "ip=" in target and target.split("ip=")[1].split("&")[0].isdigit() else 0
        ip = client_ip(writer, headers)
        refusal = None
        if Limits.max_clients and len(clients) >= Limits.max_clients:
            refusal = b"503 Service Unavailable"
        elif Limits.max_per_ip and sum(1 for c in state.values() if c.ip == ip) >= Limits.max_per_ip:
            refusal = b"429 Too Many Requests"
        elif Limits.strict and not 2 <= number <= 250:
            refusal = b"400 Bad Request"
        elif Limits.strict and any(c.number == number for c in state.values()):
            refusal = b"409 Conflict"                       # another page has this address
        if refusal:
            print("refused %s (%s)" % (ip, refusal.decode()), flush=True)
            writer.write(b"HTTP/1.1 " + refusal + b"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        me = state[writer] = Client(ip, number, time.monotonic())
        if number:
            addresses[number] = writer
            handed.pop(number, None)
        accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + WS_GUID).digest())
        writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        clients.add(writer)
        watch = asyncio.create_task(keepalive(writer, me))
        print("connected: %s%s (%d now)" % (origin, ", 10.0.5.%d" % number if number else "", len(clients)), flush=True)
        while True:
            opcode, payload = await read_message(reader)
            me.heard = time.monotonic()
            if opcode == OP_CLOSE:
                break
            if opcode == OP_PING:
                writer.write(encode(OP_PONG, payload))
            elif opcode == OP_BINARY and 14 <= len(payload) <= 1600:
                if not me.allow(len(payload)):
                    continue            # over its share: a frame lost, as on any network
                if Limits.strict and not frame_ok(me, [c for w, c in state.items() if w is not writer], payload):
                    continue
                switch[payload[6:12]] = writer
                try:
                    os.write(tap, payload)
                except (BlockingIOError, OSError):
                    pass    # the device is busy or down: a frame lost, as on any network
    except (asyncio.IncompleteReadError, asyncio.TimeoutError, asyncio.LimitOverrunError, ConnectionError):
        pass
    finally:
        if watch:
            watch.cancel()
        state.pop(writer, None)
        if writer in clients:
            clients.discard(writer)
            for table in (addresses, switch):
                for key in [k for k, v in table.items() if v is writer]:
                    del table[key]
            print("disconnected (%d left)" % len(clients), flush=True)
        writer.close()


def from_tap(tap):
    """A frame from the device goes to the client it is for; to all, if it is for all or for nobody known."""
    while True:
        try:
            frame = os.read(tap, 2048)
        except (BlockingIOError, InterruptedError):
            return
        message = encode(OP_BINARY, frame)
        to = switch.get(frame[0:6]) if len(frame) >= 6 and not frame[0] & 1 else None
        for writer in ([to] if to in clients else list(clients)):
            if writer.transport.get_write_buffer_size() < 1 << 20:  # else it is not keeping up: drop
                writer.write(message)


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--tap", default="rvtap0")
    parser.add_argument("--port", type=int, default=8001)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--origin", action="append",
                        help="an origin allowed to connect (default: http://localhost:8000 and http://127.0.0.1:8000)")
    parser.add_argument("--max-clients", type=int, default=0, help="most machines at once (0: no limit)")
    parser.add_argument("--max-per-ip", type=int, default=0, help="most machines from one address (0: no limit)")
    parser.add_argument("--rate-frames", type=int, default=0, help="frames per second one machine may send (0: no limit)")
    parser.add_argument("--rate-bytes", type=int, default=0, help="bytes per second one machine may send (0: no limit)")
    parser.add_argument("--strict", action="store_true",
                        help="a machine has one hardware address and the IP address it asked for; IPv4 and ARP only")
    parser.add_argument("--trust-forwarded", action="store_true",
                        help="the client's address is the last entry of X-Forwarded-For (a reverse proxy in front)")
    args = parser.parse_args()
    Limits.max_clients, Limits.max_per_ip = args.max_clients, args.max_per_ip
    Limits.rate_frames, Limits.rate_bytes = args.rate_frames, args.rate_bytes
    Limits.strict, Limits.trust_forwarded = args.strict, args.trust_forwarded
    origins = set(args.origin or ["http://localhost:8000", "http://127.0.0.1:8000"])

    tap = open_tap(args.tap)
    asyncio.get_running_loop().add_reader(tap, from_tap, tap)
    server = await asyncio.start_server(lambda r, w: serve(r, w, tap, origins), args.bind, args.port)
    print("relaying %s on ws://%s:%d for %s" % (args.tap, args.bind, args.port, ", ".join(sorted(origins))), flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
