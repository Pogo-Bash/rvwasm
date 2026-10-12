#!/usr/bin/env python3
"""Make an installed static library's CMake package find what its targets link (kf-static-deps.py SYSROOT NAME...).

A static KF6 library's imported target lists its private dependencies too ($<LINK_ONLY:KF6::Notifications>),
but its package does not always look for them (KF6JobWidgetsConfig does not find KF6Notifications), and a
project that uses it stops: "the link interface of target KF6::JobWidgets contains KF6::Notifications but the
target was not found". For each package NAME (a directory in lib/cmake), every target its *Targets.cmake
names that another package defines gets a find_dependency() of that package in NAMEConfig.cmake, before the
targets are included. Idempotent."""
import glob
import os
import re
import sys

root, names = sys.argv[1], sys.argv[2:]
# Targets a project made with a find module of its own for a C library (Prison's FindQRencode, FindDmtx): made
# here as imported targets of the static library in the sysroot.
CLIBS = {"QRencode": "libqrencode.a", "Dmtx": "libdmtx.a"}
cmake = os.path.join(root, "usr/lib/cmake")
owner = {}
for f in glob.glob(cmake + "/*/*Targets.cmake"):
    pkg = os.path.basename(os.path.dirname(f))
    for t in re.findall(r"add_library\(([\w:]+) ", open(f).read()):
        owner.setdefault(t, pkg)
for name in names:
    if name.startswith("Qt6") or name == "Qt6":
        continue    # Qt's own packages find what they need (and would find each other in circles)
    d = os.path.join(cmake, name)
    configs = glob.glob(d + "/*Config.cmake")
    configs = [c for c in configs if not c.endswith("ConfigVersion.cmake")]
    if not configs:
        continue
    config = configs[0]
    text = open(config).read()
    if "# nativeqt: static dependencies" in text:
        continue
    used = set()
    other = set()   # targets a build made from pkg-config or a find module, not from a package of ours
    for f in glob.glob(d + "/*Targets.cmake"):
        text_t = open(f).read()
        for t in re.findall(r"((?:KF6|Qt6|Plasma|PW|LayerShellQt|Wayland|PkgConfig|XCB|X11|QRencode|Dmtx)::\w+)", text_t):
            p = owner.get(t)
            if p and p != name:
                used.add(p)
            elif not p and t.split("::")[0] in ("PkgConfig", "Wayland", "XCB", "X11") + tuple(CLIBS):
                other.add(t)
    if not used and not other:
        continue
    block = "# nativeqt: static dependencies (kf-static-deps.py)\ninclude(CMakeFindDependencyMacro)\n" + \
        "".join("find_dependency(%s)\n" % p for p in sorted(used))
    for t in sorted(other):
        ns, comp = t.split("::")
        if ns == "PkgConfig":
            how = "find_package(PkgConfig QUIET)\n    pkg_check_modules(%s QUIET IMPORTED_TARGET %s)" % (comp, comp.lower())
        elif ns == "X11":
            how = "find_package(X11 QUIET)"
        elif ns in CLIBS:
            how = "add_library(%s STATIC IMPORTED)\n    set_target_properties(%s PROPERTIES IMPORTED_LOCATION " \
                  "\"${CMAKE_CURRENT_LIST_DIR}/../../%s\")" % (t, t, CLIBS[ns])
        else:   # Wayland::Client, XCB::XCB: ECM's find modules
            how = "find_package(ECM QUIET NO_MODULE)\n    list(APPEND CMAKE_MODULE_PATH ${ECM_FIND_MODULE_DIR})\n" \
                  "    find_package(%s QUIET COMPONENTS %s)" % (ns, comp)
        block += "if(NOT TARGET %s)\n    %s\nendif()\n" % (t, how)
    m = re.search(r'^\s*include\(.*Targets\.cmake["\)]', text, re.M)
    text = text[:m.start()] + block + text[m.start():] if m else text + "\n" + block
    open(config, "w").write(text)
    print("kf-static-deps: %s finds %s" % (name, " ".join(sorted(used))))
