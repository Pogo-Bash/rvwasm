#!/usr/bin/env python3
# Ask a vtest server (virgl_test_server) for its virgl caps v2 and write them to a file: the caps a stub
# executor hands out (gpu/tools/gpu-stub.js) until the real executor computes its own.
#   vtest-caps.py SOCKET OUT
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
def send(cmd, payload=b""):
    s.sendall(struct.pack("<II", len(payload) // 4 if cmd != 8 else len(payload), cmd) + payload)
def recv(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise SystemExit("server closed")
        b += c
    return b
name = b"vtest-caps\0"
send(8, name)                          # VCMD_CREATE_RENDERER (length in bytes)
send(10)                               # VCMD_PING_PROTOCOL_VERSION
send(7, struct.pack("<II", 0, 0))      # VCMD_RESOURCE_BUSY_WAIT handle 0
l, c = struct.unpack("<II", recv(8)); assert c == 10, c
l, c = struct.unpack("<II", recv(8)); recv(4 * l)
send(11, struct.pack("<I", 3))         # VCMD_PROTOCOL_VERSION 3
l, c = struct.unpack("<II", recv(8)); v, = struct.unpack("<I", recv(4))
send(9)                                # VCMD_GET_CAPS2
l, c = struct.unpack("<II", recv(8)); assert c == 2, c
caps = recv(l - 1)          # (the length is in bytes here, plus one: vtest_send_caps2)
open(sys.argv[2], "wb").write(caps)
print("protocol", v, "caps v2", len(caps), "bytes, max_version", struct.unpack("<I", caps[:4])[0])
