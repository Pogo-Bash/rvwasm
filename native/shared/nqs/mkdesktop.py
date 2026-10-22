#!/usr/bin/env python3
"""mkdesktop.py OUTDIR SEARCHDIRS APP...: the native programs' desktop files, under the IDs of the emulated ones they replace.

The desktop's menu, the panel's pinned launchers, "Open with" and KRunner find a program by its desktop file ID
(org.kde.konsole.desktop, systemsettings.desktop, ...). The nqs disk's share/applications comes BEFORE Debian's in XDG_DATA_DIRS
(tools/x/desktop), so a file of the same name here shadows Debian's: one entry per program, the same Name, Icon, MimeType and
Categories, and the native program started in place of the emulated one. Only the command changes: the first word of every Exec=
that names one of the native programs becomes `/opt/nq-shared/bin/nqs-run PROGRAM` (its arguments stay), TryExec= goes (it would look
for the emulated program) and DBusActivatable=true goes (D-Bus activation would start the emulated one by its service file).
SEARCHDIRS (colon separated): where the originals are, the first that has a file wins (Debian's own, then the upstream ones
installed with the build). An ID that is in none is an error."""
import glob
import os
import re
import sys

out, dirs, apps = sys.argv[1], sys.argv[2].split(":"), sys.argv[3:]
IDS = {
    "konsole": ["org.kde.konsole.desktop"],
    "dolphin": ["org.kde.dolphin.desktop"],
    "kate": ["org.kde.kate.desktop"],
    "kwrite": ["org.kde.kwrite.desktop"],
    "kcalc": ["org.kde.kcalc.desktop"],
    "systemsettings": ["systemsettings.desktop", "kdesystemsettings.desktop"],
    "okular": ["org.kde.okular.desktop", "okularApplication_*.desktop"],
    "gwenview": ["org.kde.gwenview.desktop"],
    "ark": ["org.kde.ark.desktop"],
}
unknown = [a for a in apps if a not in IDS]
if unknown:
    sys.exit("mkdesktop: no desktop file IDs known for " + " ".join(unknown))
os.makedirs(out, exist_ok=True)
names = set(apps)


def find(pattern):
    found = {}
    for d in dirs:
        for path in sorted(glob.glob(os.path.join(d, pattern))):
            found.setdefault(os.path.basename(path), path)
    return found


count = 0
for app in apps:
    for pattern in IDS[app]:
        found = find(pattern)
        if not found:
            sys.exit("mkdesktop: %s: no %s in %s" % (app, pattern, ":".join(dirs)))
        for fname, path in sorted(found.items()):
            lines = []
            for line in open(path, encoding="utf-8").read().split("\n"):
                if line.startswith("TryExec=") or re.match(r"DBusActivatable\s*=\s*true", line):
                    continue
                m = re.match(r"(Exec\s*=\s*)(\S+)(.*)$", line)
                if m and os.path.basename(m.group(2)) in names:
                    line = m.group(1) + "/opt/nq-shared/bin/nqs-run " + os.path.basename(m.group(2)) + m.group(3)
                lines.append(line)
            with open(os.path.join(out, fname), "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
            count += 1
print("mkdesktop: %d desktop files" % count)
