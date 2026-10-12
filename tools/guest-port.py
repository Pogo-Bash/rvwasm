#!/usr/bin/env python3
"""A server in the machine, reachable from the host's browser at http://localhost:PORT/.

Say python3 -m http.server 8080 runs in the Debian system of the machine in
the browser (or a Vite, Flask, ... dev server). The machine is 10.0.5.N on
the small network tools/net-relay.py makes in the build VM, so the VM can
reach it; this, run in the VM next to the relay, listens on 127.0.0.1:PORT of
the VM and passes each connection on to 10.0.5.N:PORT. Lima forwards the VM's
local ports to the Mac's, as it does the dev server's 8000, so the host's
Chrome finds the guest's server at http://localhost:PORT/.

    tools/guest-port.py 8080               the guest's 8080 at localhost:8080
    tools/guest-port.py 18080:8080 3000    the guest's 8080 at localhost:18080, its 3000 at localhost:3000
    tools/guest-port.py --machine 3 8080   the machine that is 10.0.5.3 (else: the one whose server answers)

What it lets through, and why:
- Only the ports named here, and only to the machine network (10.0.5.0/24).
  Nothing listens on more than the VM's own 127.0.0.1 (and so the Mac's
  localhost): other computers on the person's network do not see it.
- Only HTTP requests that name this computer as their host: the Host header
  must be localhost:PORT, 127.0.0.1:PORT or [::1]:PORT (the first request of
  a connection is looked at; a browser keeps a connection to one host). A web
  site the person has open elsewhere can make the browser send requests to
  localhost too, but a site that gets around the browser's checks by making
  its own name point at 127.0.0.1 ("DNS rebinding") sends its own name as the
  Host and is refused here. --any-host turns this off (for servers that are
  not HTTP: the first bytes are then not looked at).
- What else a local server is open to, this one is too: any page in the
  browser may send requests to localhost (Chrome asks the person first, for a
  public site, since its Local Network Access checks; other browsers may not),
  though it cannot read the answers unless the server allows it (CORS). As
  with any dev server on the Mac: do not serve anything secret, and stop this
  when done.
- The guest's servers get their own origin (http://localhost:PORT), not the
  dev page's (http://localhost:8000): what they serve cannot reach the page,
  its machine or its storage. (A Service Worker on the page's origin that
  passed fetches into the guest would have put guest pages on that origin.)

No dependencies.
"""
import argparse
import asyncio
import re
import time
import urllib.request

HEAD_MAX = 16384
PROBE_SECONDS = 0.5
REMEMBER_SECONDS = 10


class Machines:
    """Which machine (the last number of 10.0.5.N) has a server on a port."""

    def __init__(self, fixed, relay):
        self.fixed = fixed
        self.relay = relay
        self.found = {}  # port -> (number, when)

    async def candidates(self):
        """The machines the relay says are connected (GET /machines), else 2 to 30."""
        try:
            def ask():
                request = urllib.request.Request(self.relay + "/machines", headers={"Origin": "http://localhost:8000"})
                with urllib.request.urlopen(request, timeout=1) as answer:
                    return [int(n) for n in answer.read().decode().split()]
            numbers = await asyncio.get_running_loop().run_in_executor(None, ask)
            if numbers:
                return numbers
        except Exception:
            pass  # (an older relay without /machines)
        return list(range(2, 31))

    async def connect(self, port):
        """A connection to the guest's server on port: (reader, writer, number), or None."""
        async def first(numbers):
            for number in numbers:
                try:
                    reader, writer = await asyncio.wait_for(asyncio.open_connection("10.0.5.%d" % number, port), PROBE_SECONDS)
                except (OSError, asyncio.TimeoutError):
                    continue
                self.found[port] = (number, time.monotonic())
                return reader, writer, number
            return None

        if self.fixed:
            return await first([self.fixed])
        # The machine that answered a moment ago, else every one there is.
        number, when = self.found.pop(port, (0, 0))
        if number and time.monotonic() - when < REMEMBER_SECONDS:
            found = await first([number])
            if found:
                return found
        return await first(await self.candidates())


def allowed_host(head, port):
    """Is the request's Host this computer, with this port?"""
    match = re.search(rb"\r\nhost:[ \t]*([^\r\n]*)", head, re.IGNORECASE)
    if not match:
        return False
    host = match.group(1).strip().lower().decode("latin-1")
    names = ["localhost", "127.0.0.1", "[::1]"]
    return host in ["%s:%d" % (n, port) for n in names] + (names if port == 80 else [])


def answer(writer, status, text):
    body = (text + "\n").encode()
    writer.write(b"HTTP/1.1 " + status.encode() + b"\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: "
                 + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)


async def pipe(reader, writer):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        try:
            writer.write_eof()
        except (OSError, RuntimeError):
            pass


async def serve(reader, writer, local, guest, machines, any_host):
    peer = writer.get_extra_info("peername")
    try:
        head = b""
        if not any_host:
            try:
                head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 30)
            except (asyncio.LimitOverrunError, asyncio.IncompleteReadError, asyncio.TimeoutError):
                answer(writer, "400 Bad Request", "not an HTTP request (or its head is over %d bytes)" % HEAD_MAX)
                return
            if not allowed_host(head, local):
                print("refused a request for another host from %s: %r" % (peer, head.split(b"\r\n")[1:2]), flush=True)
                answer(writer, "421 Misdirected Request", "only requests for localhost:%d are passed to the machine" % local)
                return
        found = await machines.connect(guest)
        if not found:
            answer(writer, "502 Bad Gateway", "nothing in the machine answers on port %d (is the server running there, on 0.0.0.0?)" % guest)
            return
        g_reader, g_writer, number = found
        g_writer.write(head)
        await asyncio.gather(pipe(reader, g_writer), pipe(g_reader, writer))
        g_writer.close()
    finally:
        try:
            await writer.drain()
        except ConnectionError:
            pass
        writer.close()


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("ports", nargs="+", help="PORT, or LOCAL:GUEST")
    parser.add_argument("--machine", type=int, default=0, help="the machine's number N (10.0.5.N); else the one that answers")
    parser.add_argument("--relay", default="http://127.0.0.1:8001", help="the network relay, asked which machines there are")
    parser.add_argument("--any-host", action="store_true", help="do not look at the Host of requests (not HTTP)")
    args = parser.parse_args()
    if args.machine and not 2 <= args.machine <= 254:
        parser.error("--machine: a number from 2 to 254")
    machines = Machines(args.machine, args.relay)
    servers = []
    for spec in args.ports:
        local, _, guest = spec.partition(":")
        local, guest = int(local), int(guest or local)
        if not (0 < local < 65536 and 0 < guest < 65536):
            parser.error("no port %s" % spec)
        server = await asyncio.start_server(
            lambda r, w, l=local, g=guest: serve(r, w, l, g, machines, args.any_host), "127.0.0.1", local, limit=HEAD_MAX)
        servers.append(server)
        print("the machine's port %d at http://localhost:%d/%s" % (guest, local, " (any host)" if args.any_host else ""), flush=True)
    await asyncio.gather(*(s.serve_forever() for s in servers))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
