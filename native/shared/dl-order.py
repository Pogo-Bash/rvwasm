#!/usr/bin/env python3
"""dl-order.py USR OUT: the order in which the page compiles the nqs disk's libraries in the background (dev.html, after the
panel is up): the libraries Konsole, Dolphin and System Settings need first, then Kate and KWrite, then Okular, Gwenview and
Ark (each program's NEEDED closure, native/shared/needed-closure.py), and after each group the plugins of those programs (by
path). One library name (a file's basename, as tools/dl-manifest.py lists it) per line, no repeats; tools/dl-manifest.py --order."""
import os
import re
import subprocess
import sys

usr, out = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
groups = [
    ("konsole dolphin systemsettings", r"konsole|dolphin|kcms|packagestructure|/kio/|kiod|qml/|platforminput|imageformats|iconengines|styles/|wayland|platformthemes|kiconthemes|kwindowsystem|kfileitemaction|kded|sonnet"),
    ("kate kwrite", r"ktexteditor|kate"),
    ("okular gwenview ark", r"okular|kerfuffle|arkpart|gwenview"),
]
plugins = []
for root, _, files in os.walk(os.path.join(usr, "lib")):
    for f in files:
        if f.endswith(".so") and ("/plugins/" in root + "/" or "/qml/" in root + "/"):
            plugins.append(os.path.join(root, f))
plugins.sort()
seen, order = set(), []


def add(name):
    if name not in seen:
        seen.add(name)
        order.append(name)


for progs, pattern in groups:
    for prog in progs.split():
        path = os.path.join(usr, "bin", prog)
        if not os.path.exists(path):
            continue
        res = subprocess.run([sys.executable, os.path.join(here, "needed-closure.py"), "bin/" + prog, os.path.join(usr, "lib")],
                             cwd=usr, capture_output=True, text=True)
        for line in res.stdout.splitlines()[:-1]:
            parts = line.split()
            if len(parts) >= 2:
                add(parts[1])
    for p in plugins:
        if re.search(pattern, p):
            add(os.path.basename(p))
# whatever is left (the common plugins, the C library's helpers): last
for p in plugins:
    add(os.path.basename(p))
with open(out, "w") as f:
    f.write("\n".join(order) + "\n")
print("dl-order: %d libraries in order" % len(order))
