#!/usr/bin/env python3
"""The static plugins of other projects, for one program (static-plugins.py OUT.cmake TARGET BUILDDIR...).

A static KDE build leaves its plugins in its build tree: kcoreaddons_add_plugin installs nothing when STATIC
(KDE expects a static program to build its plugins in the same CMake project), and a static QML module
(ECM / qt_add_qml_module) keeps the objects that register it and carry its QML files (the _init and
_resources_N and _qmlcache object libraries) outside its archive. For each BUILDDIR this writes into
OUT.cmake: link every static archive and those objects into TARGET, and register what has no registration
of its own: KPluginFactory plugins (qt_static_plugin_<name>_factory) with KCoreAddons
(kRegisterStaticPluginFunction, under the namespace the plugin was built for, from the build's
KCOREADDONS_STATIC_PLUGINS<ns> cache entry), other Qt plugins (Kirigami's platform styles) with
qRegisterStaticPluginFunction. QML plugins register themselves through their _init objects."""
import glob
import os
import re
import subprocess
import sys

out, target, args = sys.argv[1], sys.argv[2], sys.argv[3:]
# BUILDDIR: everything as above; archives:BUILDDIR: its archives only (a project the program itself is part of:
# its objects are linked already); lib:NAME: -lNAME from the sysroot (a dependency the archives' own packages
# did not pass on: KIO's file widgets need KBookmarks).
dirs = [a for a in args if not a.startswith(("archives:", "lib:"))]
archive_dirs = [a[len("archives:"):] for a in args if a.startswith("archives:")]
libs = [a[len("lib:"):] for a in args if a.startswith("lib:")]
nm = os.path.join(os.environ["NQ_LLVM"], "llvm-nm")
debian_plugins = os.path.expanduser("~/rvwasm/debian-root-kde/usr/lib/riscv64-linux-gnu/qt6/plugins")

def namespace(sanitized):
    # kf6_kirigami_platform -> kf6/kirigami/platform: the split whose directory Debian has.
    parts = sanitized.split("_")
    best = sanitized.replace("_", "/")
    for mask in range(1 << (len(parts) - 1)):
        s = parts[0]
        for i, p in enumerate(parts[1:]):
            s += ("/" if mask >> i & 1 else "_") + p
        if os.path.isdir(os.path.join(debian_plugins, s)):
            return s
    return best

archives, objects, kf, qtplugins = [], [], [], []
defined = {}        # qt_static_plugin_<X> -> archive
by_target = {}      # (builddir, target name) -> the plugin classes its archive defines
aliases = []        # (name the _init object asks for, the class the archive has)
for d in dirs:
    cache = open(os.path.join(d, "CMakeCache.txt")).read() if os.path.exists(os.path.join(d, "CMakeCache.txt")) else ""
    ns_of = {}
    for m in re.finditer(r"^KCOREADDONS_STATIC_PLUGINS(\w*):INTERNAL=(.*)$", cache, re.M):
        for name in m.group(2).split(";"):
            if name:
                ns_of[name] = namespace(m.group(1))
    # Plugin archives by the plugin directory they install to (cmake_install.cmake): a KPluginFactory plugin
    # made without kcoreaddons_add_plugin (add_library(MODULE) and K_PLUGIN_CLASS_WITH_JSON: libksysguard's
    # package structure, Plasma5Support's activities and apps engines) has no <target>_factory class, and is
    # found here instead, by its archive's install destination.
    plugin_dir_of = {}
    for ci in glob.glob(d + "/**/cmake_install.cmake", recursive=True):
        for m in re.finditer(r'file\(INSTALL DESTINATION "[^"]*/plugins/([^"]+)" TYPE STATIC_LIBRARY FILES "([^"]+\.a)"\)', open(ci).read()):
            plugin_dir_of[os.path.basename(m.group(2))] = m.group(1)
    for a in sorted(glob.glob(d + "/**/*.a", recursive=True)):
        if "/CMakeFiles/" in a or "/autotests/" in a or "/tests/" in a:
            continue
        # KIO's daemons' modules and URI filters (separate processes' or optional parts; two of them
        # define the same internal category() as the other, which a static link cannot have twice)
        if re.search(r"kpasswdserver|urifilter|kuriikws|kurisearch|kiod|kssld|kioexecd|fixhosturi|localdomainuri|kshorturi", os.path.basename(a)):
            continue
        archives.append(a)
        try:
            syms = subprocess.run([nm, "--defined-only", "-g", a], capture_output=True, text=True).stdout
        except OSError:
            continue
        in_qml = "/bin/" in a or "/qml/" in a
        found = sorted(set(re.findall(r" T _Z\d+qt_static_plugin_(\w+)v$", syms, re.M)))
        for x in found:
            defined[x] = a
        by_target[(d, os.path.basename(a)[3:-2])] = [x for x in found if not x.endswith("_factory")]
        for s in found:
            if s.endswith("_factory") and s[:-len("_factory")] in {n.replace(".", "_").replace("-", "_") for n in ns_of}:
                name = next(n for n in ns_of if n.replace(".", "_").replace("-", "_") == s[:-len("_factory")])
                kf.append((s, name, ns_of[name]))
            elif (not in_qml and s.endswith("Factory") and re.match(r"(kf6/|plasma|kpackage)", plugin_dir_of.get(os.path.basename(a), ""))
                  and not plugin_dir_of[os.path.basename(a)].startswith("kf6/kirigami")):
                kf.append((s, os.path.basename(a)[3:-2], plugin_dir_of[os.path.basename(a)]))
            elif not in_qml:
                qtplugins.append(s)
    for o in sorted(glob.glob(d + "/**/CMakeFiles/*.dir/**/*.o", recursive=True, include_hidden=True)):  # (resources are under .qt/)
        t = o.split("/CMakeFiles/")[1].split(".dir/")[0]
        if "/tests/" in o or "/autotests/" in o:
            continue
        if re.search(r"(_init|_resources_\d+|_qmlcache)$", t):
            objects.append(o)
            if t.endswith("_init"):
                # A static QML module whose qmldir names the generated plugin class (org_kde_..._Plugin) while
                # its plugin is a class of its own (libplasma's CoreBindingsPlugin, ...): Qt's _init object
                # asks for the name the qmldir has. Answered below by an alias to the class the archive has.
                u = subprocess.run([nm, "-u", o], capture_output=True, text=True).stdout
                for want in re.findall(r"_Z\d+qt_static_plugin_(\w+)v", u):
                    aliases.append((want, d, t[:-len("_init")]))
        elif o.endswith("_qmltyperegistrations.cpp.o"):
            # A module's QML_ELEMENT types are registered by a static object in this file, which nothing
            # refers to: from an archive it would never be linked (PlasmaCore.ToolTipArea "is not a type").
            objects.append(o)

# A class name defined by more than one archive (KIO's workers are all KIOPluginForMetaData) cannot be
# referred to: those are left out (and reported).
dups = sorted({q for q in qtplugins if qtplugins.count(q) > 1})
qtplugins = sorted(set(qtplugins) - set(dups))
if dups:
    print("static-plugins: left out (defined more than once): " + " ".join(dups))
alias_pairs = []
for want, d, target_name in aliases:
    if want in defined:
        continue
    have = by_target.get((d, target_name), [])
    if len(have) == 1:
        alias_pairs.append((want, have[0]))
    else:
        print("static-plugins: no plugin class for %s (%s: %s)" % (want, target_name, have))
for d in archive_dirs:
    for a in sorted(glob.glob(d + "/**/*.a", recursive=True)):
        if "/CMakeFiles/" not in a and "/autotests/" not in a and "/tests/" not in a:
            archives.append(a)
gen = os.path.splitext(out)[0] + ".cpp"
with open(gen, "w") as f:
    f.write("// Generated by native/static-plugins.py.\n#include <QPluginLoader>\n#include <QString>\n#include <QtPlugin>\n")
    f.write("extern void kRegisterStaticPluginFunction(const QString &, const QString &, QStaticPlugin);\n")
    for s, name, ns in kf:
        f.write("extern const QStaticPlugin qt_static_plugin_%s();\n" % s)
    for s in qtplugins:
        f.write("extern const QStaticPlugin qt_static_plugin_%s();\n" % s)
    for want, have in alias_pairs:
        f.write("extern const QStaticPlugin qt_static_plugin_%s();\n" % have)
        f.write("const QStaticPlugin qt_static_plugin_%s() { return qt_static_plugin_%s(); }\n" % (want, have))
    f.write("namespace {\nstruct NqStaticPlugins {\n    NqStaticPlugins() {\n")
    for s, name, ns in kf:
        f.write('        kRegisterStaticPluginFunction(QStringLiteral("%s"), QStringLiteral("%s"), qt_static_plugin_%s());\n' % (name, ns, s))
        if ns == "kf6/kio":
            # (KIO looks for a static worker in Qt's list, by its IID: patches/nativeqt-kio.diff)
            f.write("        qRegisterStaticPluginFunction(qt_static_plugin_%s());\n" % s)
    for s in qtplugins:
        f.write("        qRegisterStaticPluginFunction(qt_static_plugin_%s());\n" % s)
    f.write("    }\n} nqStaticPlugins;\n}\n")
with open(out, "w") as f:
    f.write("# Generated by native/static-plugins.py: %d archives, %d objects, %d KF plugins, %d Qt plugins.\n"
            % (len(archives), len(objects), len(kf), len(qtplugins)))
    f.write("target_sources(%s PRIVATE %s)\n" % (target, gen))
    # (The X11 libraries that Plasma's X11 code links: their own packages do not pass them on when static.)
    xl = [l for l in ("xcb-composite", "xcb-damage", "xcb-xfixes", "xcb-render", "xcb-randr", "xcb-shape", "xcb-shm",
                      "xcb-image", "xcb-keysyms", "xcb-res", "xcb-icccm", "xcb-ewmh", "xcb-util", "xcb-render-util",
                      "xcb-cursor", "xcb-xkb", "xcb-sync", "X11-xcb", "Xcursor", "Xi", "Xtst", "xkbfile", "Xfixes", "Xrender", "Xext", "X11", "xcb", "Xau", "Xdmcp")
          if os.path.exists(os.path.join(os.environ.get("NQ_SYSROOT", ""), "usr/lib/lib%s.a" % l))]
    f.write("target_link_libraries(%s PRIVATE %s)\n" % (target, " ".join("-l" + l for l in xl)))
    # (Kirigami's and Plasma's styles use Qt Quick Controls' style API.)
    f.write("find_package(Qt6 REQUIRED COMPONENTS QuickControls2)\ntarget_link_libraries(%s PRIVATE Qt6::QuickControls2)\n" % target)
    # (and the Qt modules the full build's parts use, where they are built: KScreen's Sensors, night light's
    # Positioning, KNotifyConfig's and Prison's Multimedia)
    for m in ("Sensors", "Positioning", "Multimedia"):
        f.write("find_package(Qt6 QUIET COMPONENTS %s)\nif(TARGET Qt6::%s)\n    target_link_libraries(%s PRIVATE Qt6::%s)\nendif()\n"
                % (m, m, target, m))
    # (not their plugins: a sensor backend for iio-sensor-proxy has KIO's D-Bus properties interface class too,
    # and none of them has a device here)
    f.write("qt_import_plugins(%s EXCLUDE_BY_TYPE sensors position multimedia)\n" % target)

    f.write("target_link_libraries(%s PRIVATE\n%s)\n" % (target, "".join("    %s\n" % x for x in objects + archives + ["-l" + l for l in libs])))
print("static-plugins: aliases " + " ".join("%s=%s" % p for p in alias_pairs))
print("static-plugins: %d archives, %d objects, KF plugins: %s; Qt plugins: %s" % (
    len(archives), len(objects), " ".join("%s(%s)" % (n, ns) for s, n, ns in kf), " ".join(qtplugins)))
