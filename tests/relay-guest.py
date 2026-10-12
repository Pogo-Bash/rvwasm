#!/usr/bin/env python3
"""A stand-in for the page, to try the relay (and its firewall) without a browser: it makes a network namespace that
is "the machine", connects it to the relay's WebSocket as a page does (the Ethernet frames of a TAP device inside the
namespace go over the socket and back), and runs a command in it.

    sudo tests/relay-guest.py --url 127.0.0.1:8081 --origin http://localhost:8080 [--ip N] [--dns 10.0.5.1] -- COMMAND...

--same-origin asks /address the way a page on the relay's own address does (no Origin header; the WebSocket
still names it). Several can run at once: give each its own --ns.

--url also takes a public relay, https://HOST[/relay] (TLS; the Origin must then be the site's own, https://HOST).

Example:  ... -- sh -c 'getent hosts deb.debian.org; curl -sI https://deb.debian.org/ | head -1'
The namespace's /etc/resolv.conf is the given --dns (what dev.html's dns= does for /init). Needs root, `ip` and Linux.
"""
import argparse
import base64
import fcntl
import os
import select
import socket
import ssl
import struct
import subprocess
import sys

TUNSETIFF, IFF_TAP, IFF_NO_PI = 0x400454CA, 0x0002, 0x1000


def ws_frame(payload, first=0x82):
    mask = os.urandom(4)
    n = len(payload)
    head = bytes([first]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
    return head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--url", default="127.0.0.1:8081")
    p.add_argument("--origin", default="http://localhost:8080")
    p.add_argument("--ip", type=int, default=0, help="last number of 10.0.5.N (default: ask the relay)")
    p.add_argument("--dns", default="10.0.5.1")
    p.add_argument("--ns", default="rvguest")
    p.add_argument("--same-origin", action="store_true", help="/address without an Origin header")
    p.add_argument("cmd", nargs="+")
    a = p.parse_args()
    tls = a.url.startswith(("https://", "wss://"))
    if tls:
        hostpart = a.url.split("://", 1)[1]
        host, _, prefix = hostpart.partition("/")
        prefix = "/" + prefix.rstrip("/") if prefix.strip("/") else ""
        port = "443"
    else:
        host, port = a.url.rsplit(":", 1)
        prefix = ""

    def connect():
        s = socket.create_connection((host, int(port)))
        return ssl.create_default_context().wrap_socket(s, server_hostname=host) if tls else s

    def http(path):
        s = connect()
        s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\n%sConnection: close\r\n\r\n"
                   % (prefix + path, host, "" if a.same_origin else "Origin: %s\r\n" % a.origin)).encode())
        data = b""
        while chunk := s.recv(4096):
            data += chunk
        head, body = data.split(b"\r\n\r\n", 1)
        if b" 200 " not in head.split(b"\r\n")[0]:
            sys.exit("relay refused " + path + ": " + head.split(b"\r\n")[0].decode())
        return body.decode()

    n = a.ip or int(http("/address"))
    sock = connect()
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET %s/?ip=%d HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (prefix, n, host, a.origin, key)).encode())
    reply = b""
    while b"\r\n\r\n" not in reply:
        reply += sock.recv(1)
    if b" 101 " not in reply.split(b"\r\n")[0]:
        sys.exit("relay refused: " + reply.split(b"\r\n")[0].decode())

    tap = os.open("/dev/net/tun", os.O_RDWR | os.O_NONBLOCK)
    tapname = "rvg%d" % os.getpid()              # (its own name: several of these can run at once)
    fcntl.ioctl(tap, TUNSETIFF, struct.pack("16sH", tapname.encode(), IFF_TAP | IFF_NO_PI))
    ns = a.ns
    subprocess.run(["ip", "netns", "del", ns], stderr=subprocess.DEVNULL)
    subprocess.run(["ip", "netns", "add", ns], check=True)
    subprocess.run(["ip", "link", "set", tapname, "netns", ns], check=True)
    sh = lambda c: subprocess.run(["ip", "netns", "exec", ns, "sh", "-c", c], check=True)
    sh("ip link set lo up; ip link set %s name eth0; ip addr add 10.0.5.%d/24 dev eth0; ip link set eth0 up; "
       "ip route add default via 10.0.5.1" % (tapname, n))
    os.makedirs("/etc/netns/%s" % ns, exist_ok=True)
    open("/etc/netns/%s/resolv.conf" % ns, "w").write("nameserver %s\n" % a.dns)

    child = subprocess.Popen(["ip", "netns", "exec", ns] + a.cmd)
    buf = b""
    try:
        while child.poll() is None:
            r, _, _ = select.select([tap, sock], [], [], 0.2)
            if tls and sock.pending():
                r.append(sock)
            if tap in r:
                try:
                    while True:
                        sock.sendall(ws_frame(os.read(tap, 2048)))
                except BlockingIOError:
                    pass
            if sock in r:
                data = sock.recv(65536)
                if not data:
                    break
                buf += data
                while len(buf) >= 2:                      # (the relay sends unmasked binary frames)
                    ln, off = buf[1] & 0x7F, 2
                    if ln == 126:
                        if len(buf) < 4: break
                        ln, off = struct.unpack(">H", buf[2:4])[0], 4
                    elif ln == 127:
                        if len(buf) < 10: break
                        ln, off = struct.unpack(">Q", buf[2:10])[0], 10
                    if len(buf) < off + ln:
                        break
                    if buf[0] & 0x0F == 2:
                        os.write(tap, buf[off:off + ln])
                    elif buf[0] & 0x0F == 9:                  # a ping from the relay: answer it, or it hangs up
                        sock.sendall(ws_frame(buf[off:off + ln], 0x8A))
                    buf = buf[off + ln:]
    finally:
        child.kill() if child.poll() is None else None
        subprocess.run(["ip", "netns", "del", ns], stderr=subprocess.DEVNULL)
    sys.exit(child.returncode or 0)


if __name__ == "__main__":
    main()
