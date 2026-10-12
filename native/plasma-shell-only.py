#!/usr/bin/env python3
"""plasma-workspace 6.3.6, cut down to plasmashell (and what it links: libkworkspace, and the parts in
plasma-workspace-extra.cmake) for a native build: only what plasmashell links is required; everything else
plasma-workspace looks for becomes optional. Idempotent (marker lines). For a fresh source tree.
    plasma-shell-only.py SRCDIR"""
import re
import sys

src = sys.argv[1]
p = src + "/CMakeLists.txt"
s = open(p).read()
MARK = "# nativeqt: plasmashell only (plasma-shell-only.py)\n"

OLD_IMPORTS = """qt_import_plugins(plasmashell INCLUDE Qt6::QWaylandIntegrationPlugin Qt6::QWaylandXdgShellIntegrationPlugin
    Qt6::QWaylandEglClientBufferPlugin Qt6::QGifPlugin Qt6::QICOPlugin Qt6::QJpegPlugin Qt6::QSvgPlugin
    Qt6::QSvgIconPlugin)
"""
NEW_IMPORTS = """set(_nq_qt_plugins Qt6::QWaylandIntegrationPlugin Qt6::QWaylandXdgShellIntegrationPlugin
    Qt6::QWaylandEglClientBufferPlugin Qt6::QGifPlugin Qt6::QICOPlugin Qt6::QJpegPlugin Qt6::QSvgPlugin
    Qt6::QSvgIconPlugin)
# (qtimageformats': WebP wallpapers and previews, and the others it builds here)
foreach(_p QWebpPlugin QTgaPlugin QWbmpPlugin QICNSPlugin QTiffPlugin)
    if(TARGET Qt6::${_p})
        list(APPEND _nq_qt_plugins Qt6::${_p})
    endif()
endforeach()
qt_import_plugins(plasmashell INCLUDE ${_nq_qt_plugins})
"""


def static_plugins():
    """plasmashell links its plugins (static program): Qt's Wayland platform and shell integration, image
    formats, the QML modules its QML (Debian's, at run time) imports, its own package structures, and the
    other projects' static plugins listed in ${NQ_STATIC_PLUGINS} (native/static-plugins.py)."""
    q = src + "/shell/CMakeLists.txt"
    t = open(q).read()
    if "# nativeqt: static plugins" in t:
        if "_nq_qt_plugins" not in t:
            t = t.replace(OLD_IMPORTS, NEW_IMPORTS)
            open(q, "w").write(t)
            print("plasma-shell-only: image format plugins added")
        return
    # All of its links with the keyword form (Qt's and KCoreAddons' import functions use it, and CMake allows
    # only one form per target).
    t = re.sub(r"target_link_libraries\(plasmashell(\s+)(?!PRIVATE|PUBLIC|INTERFACE)", r"target_link_libraries(plasmashell PRIVATE\1", t)
    t += """
# nativeqt: static plugins (plasma-shell-only.py)
set(_nq_qt_plugins Qt6::QWaylandIntegrationPlugin Qt6::QWaylandXdgShellIntegrationPlugin
    Qt6::QWaylandEglClientBufferPlugin Qt6::QGifPlugin Qt6::QICOPlugin Qt6::QJpegPlugin Qt6::QSvgPlugin
    Qt6::QSvgIconPlugin)
# (qtimageformats': WebP wallpapers and previews, and the others it builds here)
foreach(_p QWebpPlugin QTgaPlugin QWbmpPlugin QICNSPlugin QTiffPlugin)
    if(TARGET Qt6::${_p})
        list(APPEND _nq_qt_plugins Qt6::${_p})
    endif()
endforeach()
qt_import_plugins(plasmashell INCLUDE ${_nq_qt_plugins})
if(NQ_STATIC_PLUGINS)
    include(${NQ_STATIC_PLUGINS})
endif()
"""
    open(q, "w").write(t)
    print("plasma-shell-only: static plugins added")

def desktop_worker_static():
    """KIO's desktop:/ worker (the folder view's desktop) as a static plugin that runs in a thread of
    plasmashell (KIO's in-thread path, patches/nativeqt-kio.diff): its metadata class made a KIO
    WorkerFactory, named as kcoreaddons_add_plugin names a factory."""
    p = src + "/kioworkers/desktop/kio_desktop.cpp"
    t = open(p).read()
    if "kio_desktop_factory" in t:
        return
    old = """// Pseudo plugin class to embed meta data
class KIOPluginForMetaData : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.kio.worker.desktop" FILE "desktop.json")
};"""
    assert old in t
    t = t.replace(old, """// nativeqt: the plugin class a factory, so that a static program runs the worker in a thread of its own
class kio_desktop_factory : public KIO::WorkerFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.kio.worker.desktop" FILE "desktop.json")
public:
    std::unique_ptr<KIO::WorkerBase> createWorker(const QByteArray &pool, const QByteArray &app) override
    {
        return std::make_unique<DesktopProtocol>(QByteArrayLiteral("desktop"), pool, app);
    }
};""")
    t = t.replace("#include <KDirNotify>\n", "#include <KDirNotify>\n#include <KIO/WorkerFactory>\n", 1)
    a = 'extern "C" {\nint Q_DECL_EXPORT kdemain(int argc, char **argv)\n'
    i = t.index(a)
    j = t.index("\n}\n}\n", i) + 5
    t = t[:i] + "#ifndef QT_STATICPLUGIN // nativeqt: a static worker runs in the program's threads, no kdemain (one per worker)\n" + t[i:j] + "#endif\n" + t[j:]
    open(p, "w").write(t)
    print("plasma-shell-only: desktop worker a factory")

def extra_parts():
    """The other parts of plasma-workspace that plasmashell links (native/plasma-workspace-extra.cmake, after
    the shell) and their linking into it (native/plasma-workspace-end.cmake, at the end); NQ_CMAKE_DIR is
    native/ (the build passes it)."""
    t = open(p).read()
    if "plasma-workspace-extra.cmake" in t:
        return
    t = t.replace("add_subdirectory(shell)\n", "add_subdirectory(shell)\ninclude(${NQ_CMAKE_DIR}/plasma-workspace-extra.cmake)  # nativeqt\n", 1)
    t = t.replace("feature_summary(WHAT ALL", "include(${NQ_CMAKE_DIR}/plasma-workspace-end.cmake)  # nativeqt\nfeature_summary(WHAT ALL", 1)
    open(p, "w").write(t)
    print("plasma-shell-only: extra parts included")


def clock_without_icu():
    """The digital clock uses ICU only for the city names in its time-zone picker; there is no ICU in the
    sysroot, so without it the picker shows the zone id (what the applet already does when ICU fails)."""
    d = src + "/applets/digital-clock/plugin/"
    c = open(d + "CMakeLists.txt").read()
    if "nativeqt" in c:
        return
    c = c.replace("        ICU::i18n\n        ICU::uc\n", "")
    c += "if(TARGET ICU::i18n)  # nativeqt\n    target_link_libraries(digitalclockplugin PRIVATE ICU::i18n ICU::uc)\nendif()\n"
    open(d + "CMakeLists.txt", "w").write(c)
    h = open(d + "timezonesi18n.h").read()
    h = h.replace("#include <unicode/tznames.h>\n", "#if __has_include(<unicode/tznames.h>)  // nativeqt: no ICU\n#define NQ_ICU 1\n#include <unicode/tznames.h>\n#endif\n")
    h = h.replace("    QScopedPointer<icu::TimeZoneNames> m_tzNames;\n", "#ifdef NQ_ICU\n    QScopedPointer<icu::TimeZoneNames> m_tzNames;\n#endif\n")
    open(d + "timezonesi18n.h", "w").write(h)
    t = open(d + "timezonesi18n.cpp").read()
    t = t.replace("#include <unicode/localebuilder.h>\n", "#ifdef NQ_ICU\n#include <unicode/localebuilder.h>\n#endif\n")
    t = t.replace("    if (!m_tzNames) {\n        return timezoneId;\n    }\n", "#ifndef NQ_ICU\n    return timezoneId;\n#else\n    if (!m_tzNames) {\n        return timezoneId;\n    }\n", 1)
    t = t.replace("QStringView(cityName.getBuffer(), cityName.length()).toString();\n}\n", "QStringView(cityName.getBuffer(), cityName.length()).toString();\n#endif\n}\n", 1)
    t = t.replace("    const auto locale = icu::Locale(", "#ifdef NQ_ICU\n    const auto locale = icu::Locale(", 1)
    t = t.replace("u_errorName(error);\n    }\n", "u_errorName(error);\n    }\n#endif\n", 1)
    open(d + "timezonesi18n.cpp", "w").write(t)
    print("plasma-shell-only: digital clock without ICU")


def tray_unavailable():
    """The system tray leaves out the applets listed in NQ_TRAY_UNAVAILABLE (plasma-workspace-extra.cmake): what
    this build cannot load would otherwise be an error icon in the tray."""
    q = src + "/applets/systemtray/plasmoidregistry.cpp"
    t = open(q).read()
    if "NQ_TRAY_UNAVAILABLE" in t:
        return
    old = """    const QString &pluginId = pluginMetaData.pluginId();

    m_systrayApplets[pluginId] = pluginMetaData;"""
    assert old in t
    t = t.replace(old, """    const QString &pluginId = pluginMetaData.pluginId();
#ifdef NQ_TRAY_UNAVAILABLE
    // nativeqt: an applet this build has not the parts of is not offered (it would be an error icon)
    if (QStringLiteral(NQ_TRAY_UNAVAILABLE).split(u',').contains(pluginId)) {
        return;
    }
#endif

    m_systrayApplets[pluginId] = pluginMetaData;""", 1)
    open(q, "w").write(t)
    print("plasma-shell-only: tray without the unavailable applets")


def klipper_software_delegate():
    """Klipper's list marks the current item (the one under the pointer) by drawing its text through an
    OpacityMask (Qt5Compat.GraphicalEffects: a ShaderEffect, to fade the text out behind the item's tool buttons)
    and hiding the plain label. Qt Quick's software renderer, which plasmashell runs with here, draws no
    ShaderEffect: the current item's text vanished (a highlight with nothing on it). With the software renderer
    the label stays, narrowed by the tool buttons instead of faded behind them."""
    q = src + "/klipper/declarative/qml/ClipboardItemDelegate.qml"
    t = open(q).read()
    if "nativeqt" in t:
        return
    pairs = [
        ("    readonly property bool shouldUseOverflowButton:",
         "    // nativeqt: no ShaderEffect (OpacityMask) with Qt Quick's software renderer\n"
         "    readonly property bool softwareRender: GraphicsInfo.api === GraphicsInfo.Software\n"
         "    readonly property bool shouldUseOverflowButton:"),
        ("        visible: !!source && menuItem.ListView.isCurrentItem\n",
         "        visible: !!source && menuItem.ListView.isCurrentItem && !menuItem.softwareRender\n"),
        ("        visible: !menuItem.ListView.isCurrentItem\n        anchors {",
         "        visible: !menuItem.ListView.isCurrentItem || menuItem.softwareRender\n        anchors {"),
        ("            rightMargin: expandButtonLoader.implicitWidth + expandButtonLoader.anchors.rightMargin\n",
         "            rightMargin: expandButtonLoader.implicitWidth + expandButtonLoader.anchors.rightMargin\n"
         "                + (menuItem.softwareRender && menuItem.ListView.isCurrentItem && toolButtonsLoader.active ? toolButtonsLoader.implicitWidth : 0)\n"),
    ]
    for old, new in pairs:
        assert old in t, old
        t = t.replace(old, new, 1)
    open(q, "w").write(t)
    print("plasma-shell-only: Klipper's list text without OpacityMask under the software renderer")


def sysmon_face_controller_notify():
    """The system monitor widgets' QML (Debian's) reads Plasmoid.faceController, which the C++ applet makes in
    init() and declares CONSTANT. libplasma makes the applet's QML before it calls init() (ContainmentItem
    appletAddedForward, on appletAboutToBeAdded), the QML's first reading found null, FullRepresentation.qml
    (no ?. there) threw, and as a CONSTANT property never tells anyone, the face never appeared: blank widgets
    with no numbers and no chart. Declared with a change signal that init() emits, the bindings run again."""
    h = src + "/applets/systemmonitor/systemmonitor/systemmonitor.h"
    c = src + "/applets/systemmonitor/systemmonitor/systemmonitor.cpp"
    t = open(h).read()
    if "faceControllerChanged" in t:
        return
    old = "Q_PROPERTY(KSysGuard::SensorFaceController *faceController READ faceController CONSTANT)"
    assert old in t
    t = t.replace(old, "// nativeqt: NOTIFY (the QML is made before init(); plasma-shell-only.py)\n"
                       "    Q_PROPERTY(KSysGuard::SensorFaceController *faceController READ faceController NOTIFY faceControllerChanged)", 1)
    old = "public Q_SLOTS:\n    void configChanged() override;\n"
    assert old in t
    t = t.replace(old, "Q_SIGNALS:\n    void faceControllerChanged();\n\n" + old, 1)
    open(h, "w").write(t)
    t = open(c).read()
    old = "    m_sensorFaceController->loadPreset(preset);\n}\n"
    assert old in t
    t = t.replace(old, "    m_sensorFaceController->loadPreset(preset);\n    Q_EMIT faceControllerChanged();\n}\n", 1)
    open(c, "w").write(t)
    print("plasma-shell-only: system monitor applet's faceController notifies")


static_plugins()
clock_without_icu()
tray_unavailable()
klipper_software_delegate()
sysmon_face_controller_notify()
if MARK in s:
    extra_parts()
    desktop_worker_static()
    print("plasma-shell-only: already")
    sys.exit(0)

# Qt: what plasmashell and libkworkspace use; the rest optional.
s = re.sub(r"find_package\(Qt6 \$\{QT_MIN_VERSION\} CONFIG REQUIRED COMPONENTS\s+Concurrent Core5Compat DBus Network Positioning Quick QuickWidgets\s+ShaderTools Sql Svg Widgets\)",
           "find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Concurrent DBus Network Quick Svg Widgets WaylandClient)\n"
           "find_package(Qt6 ${QT_MIN_VERSION} CONFIG OPTIONAL_COMPONENTS Core5Compat Positioning QuickWidgets ShaderTools Sql)", s)

# KF6: plasmashell's own list required, the rest optional.
need = "Config CoreAddons Crash DBusAddons GlobalAccel GuiAddons I18n KIO Package Notifications Service Solid StatusNotifierItem Svg WidgetsAddons WindowSystem XmlGui"
s = re.sub(r"find_package\(KF6 \$\{KF6_MIN_VERSION\} REQUIRED COMPONENTS.*?\)",
           "find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS " + need + ")\n"
           "find_package(KF6 ${KF6_MIN_VERSION} OPTIONAL_COMPONENTS Archive Auth Declarative IconThemes ItemModels KCMUtils KDED NewStuff\n"
           "    NotifyConfig Parts Prison Runner TextEditor TextWidgets UnitConversion Wallet DocTools)", s, count=1, flags=re.S)

keep = {"ECM", "Qt6", "KF6", "Plasma", "PlasmaQuick", "KWayland", "PlasmaActivities", "LayerShellQt",
        "PlasmaWaylandProtocols", "Wayland", "PkgConfig"}
out = []
for line in s.split("\n"):
    m = re.match(r"\s*find_package\((\w+)", line)
    if m and m.group(1) not in keep:
        line = line.replace(" REQUIRED", "")
    if re.match(r"\s*pkg_check_modules\(", line):
        line = line.replace(" REQUIRED", "")
    line = line.replace("TYPE REQUIRED", "TYPE OPTIONAL")
    out.append(line)
s = "\n".join(out)

# Build libkworkspace and shell only.
lines = s.split("\n")
start = next(i for i, l in enumerate(lines) if l.startswith("add_subdirectory(login-sessions)"))
res = []
for i, l in enumerate(lines):
    st = l.strip()
    if i >= start and (st.startswith("add_subdirectory(") or st.startswith("ecm_optional_add_subdirectory(")) \
            and st not in ("add_subdirectory(libkworkspace)", "add_subdirectory(shell)"):
        res.append("# " + l)
    elif i >= start and st.startswith("kdoctools_install("):
        res.append("# " + l)
    else:
        res.append(l)
s = MARK + "\n".join(res)
# libkworkspace's ScreenSaver interface comes from KScreenLocker's package (not built here): its XML, as
# kscreenlocker installs it, is put in nativeqt-dbus/ (plasma-shell-only.py's caller copies it) when the
# package is missing.
s = s.replace("add_subdirectory(libkworkspace)",
    "if(NOT KSCREENLOCKER_DBUS_INTERFACES_DIR)\n    set(KSCREENLOCKER_DBUS_INTERFACES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/nativeqt-dbus)\nendif()\n"
    "add_subdirectory(libkworkspace)", 1)
open(p, "w").write(s)

# shell: no autotests, no kconf_update scripts (they need more of KF6).
q = src + "/shell/CMakeLists.txt"
t = open(q).read()
t = t.replace("add_subdirectory(kconf_update)", "# add_subdirectory(kconf_update)  # nativeqt")
open(q, "w").write(t)
print("plasma-shell-only: done")
extra_parts()
desktop_worker_static()
