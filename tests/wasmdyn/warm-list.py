#!/usr/bin/env python3
"""warm-list.py LOG MANIFEST AFTER UNTIL [-o docker/boot/NAME-warm.json]: the pieces of a disk that the native programs' first starts read,
for the page's background fetch (`warm` in the disk's manifest.json: tools/r2-publish.py puts it there from docker/boot/NAME-warm.json,
tools/www/dev.html warmDisks fetches the pieces once the desktop is up and the machine idle).

LOG is a tests/wasmdyn/serve-local-site.py request log (SITE_LOG: epoch seconds, class, piece hash prefix, bytes) of a cold visit; MANIFEST
the manifest of the disk the visit used (its `hashes`); AFTER and UNTIL epoch seconds: the first click on a program and the last window
(tests/wasmdyn/native-chrome.js prints them as "EPOCH"). The pieces of that disk requested in between, except the ones the manifest's own
boot list has (those the boot fetches anyway), in the order first requested, as {"id", "warm": [piece numbers], "hashes": [their sha256]}."""
import json, sys

# --boot: write the list as `boot` (what the visit read from its start until UNTIL, the desktop's panel: AFTER = the visit's start) instead of `warm`
# (docker/boot/NAME.json instead of NAME-warm.json; used for the nqs disk once it holds plasmashell)
key = "boot" if "--boot" in sys.argv else "warm"
args = [a for a in sys.argv[1:] if a not in ("-o", "--boot")]
out = None
if "-o" in sys.argv:
    out = sys.argv[sys.argv.index("-o") + 1]
    args.remove(out)
log, manifest, after, until = args[0], args[1], float(args[2]), float(args[3])
m = json.load(open(manifest))
by = {}
for i, h in enumerate(m["hashes"]):
    if h:
        by.setdefault(h[:8], []).append(i)
boot = set(m.get("boot", [])) if key == "warm" else set()
seen, warm = set(), []
for line in open(log):
    t, kind, name, _ = line.split()
    t = float(t)
    if not (after <= t <= until) or not kind.startswith("disk-") or name not in by:
        continue
    for i in by[name]:
        if i not in boot and i not in seen:
            seen.add(i)
            warm.append(i)
doc = {"disk": m.get("name", ""), "id": m["id"], key: warm, "hashes": [m["hashes"][i] for i in warm]}
text = json.dumps(doc) + "\n"
if out:
    open(out, "w").write(text)
print("%d pieces (%d MiB) not in the boot list, requested between %.0f and %.0f" % (len(warm), len(warm), after, until), file=sys.stderr)
