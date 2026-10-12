# In the machine (inside `debian`, as root): a GPU window's frames, from the window server's table and the GPU
# server's ring read through /proc/PID/mem, while it draws: when each present was committed (c), when the ring's
# done passed its gpu_serial (r), when the page said it had it (u: uploaded) and drew it (s: shown), and how
# often the page draws at all. A page that does not draw (the tab hidden: page_hidden > 0) leaves u and s to the
# window server's stand-in.   python3 stageb-trace.py [SECONDS] [TITLE]
import struct, time, sys, re, subprocess
log = open("/root/wlserver.log", "rb").read().decode("latin1")
tab = int(re.findall(r"table: at (\d+)", log)[-1]); wpid = int(re.findall(r"table: at \d+ \(.*?\), pid (\d+)", log)[-1])
rl = open("/root/rvgpu.log", "rb").read().decode("latin1")
ring = int(re.findall(r"ring at (\d+)", rl)[-1])
gpid = int(subprocess.run(["pgrep", "-x", "rvgpu"], capture_output=True, text=True).stdout.split()[0])
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8
title = (sys.argv[2] if len(sys.argv) > 2 else "es2gears").encode()
f = open("/proc/%d/mem" % wpid, "rb", buffering=0); g = open("/proc/%d/mem" % gpid, "rb", buffering=0)
def rd(fh, at, n):
    fh.seek(at); return fh.read(n)
slot = None
for i in range(64):
    s = rd(f, tab + 2048 + i * 384, 384)
    if s[4:8] != b"\0\0\0\0" and title in s[256:320] and struct.unpack_from("<I", s, 72)[0] == 2:
        slot = i
if slot is None:
    print("no GPU slot with that title"); sys.exit(1)
at = tab + 2048 + slot * 384
t0 = time.monotonic(); end = t0 + secs
print("page_hidden %d" % struct.unpack("<I", rd(f, tab + 44, 4))[0])
ev = {}; pf_last = None; pf_times = []; loops = 0; last_present = None
while True:
    now = time.monotonic()
    if now > end: break
    loops += 1
    h = rd(f, tab + 64, 8); pf = struct.unpack("<I", h[:4])[0]
    if pf != pf_last:
        pf_times.append(now); pf_last = pf
    s = rd(f, at, 248)
    present = struct.unpack_from("<I", s, 56)[0]; up, sh = struct.unpack_from("<II", s, 224); ser = struct.unpack_from("<I", s, 244)[0]
    done = struct.unpack("<I", rd(g, ring + 68, 4))[0]
    if present != last_present:
        ev.setdefault(present, {})["c"] = now; ev[present]["ser"] = ser; last_present = present
    for p, e in ev.items():
        if "c" not in e: continue
        if "r" not in e and ((done - e["ser"]) & 0xffffffff) < 0x80000000: e["r"] = now
        if "u" not in e and up >= p: e["u"] = now
        if "s" not in e and sh >= p: e["s"] = now
ms = lambda a, b: (b - a) * 1000
full = [e for p, e in sorted(ev.items()) if all(k in e for k in "crus")][1:]
print("loops %d (%.2f ms each), page frames %d in %.1f s (%.1f ms apart)" % (loops, ms(t0, end) / loops, len(pf_times), end - t0, ms(pf_times[0], pf_times[-1]) / max(1, len(pf_times) - 1)))
print("presents %d complete %d" % (len(ev), len(full)))
for k1, k2 in (("c", "r"), ("r", "u"), ("u", "s"), ("c", "s")):
    v = sorted(ms(e[k1], e[k2]) for e in full)
    if v: print("%s->%s: median %.1f ms, min %.1f, max %.1f" % (k1, k2, v[len(v) // 2], v[0], v[-1]))
nxt = []
ps = sorted(p for p in ev if "c" in ev[p])
for a, b in zip(ps, ps[1:]):
    if "s" in ev[a]: nxt.append(ms(ev[a]["s"], ev[b]["c"]))
nxt.sort()
if nxt: print("s->next c: median %.1f ms, min %.1f, max %.1f" % (nxt[len(nxt)//2], nxt[0], nxt[-1]))
for p in ps[3:9]:
    e = ev[p]; print(p, " ".join("%s=%.1f" % (k, ms(t0, e[k])) for k in "crus" if k in e))
