# nativeqt: plasmashell links what this project built as static plugins, and what Debian's Plasma QML imports
# (included before feature_summary by plasma-shell-only.py).
if(NQ_QML_SCAN)
    # Qt 6.8's import scan looks at the target's own source directory only (PATH_TO_SCAN is ignored): a library
    # of nothing whose source directory is the copy of Debian's Plasma QML is scanned instead, and what it
    # imports (Qt's QML plugins, their init and resource objects) is linked through it.
    file(WRITE ${NQ_QML_SCAN}/nq_qml_imports.cpp "")
    file(WRITE ${NQ_QML_SCAN}/CMakeLists.txt "add_library(nq_qml_imports STATIC nq_qml_imports.cpp)\ntarget_link_libraries(nq_qml_imports PRIVATE Qt6::Qml)\nqt_import_qml_plugins(nq_qml_imports)\n")
    add_subdirectory(${NQ_QML_SCAN} ${CMAKE_CURRENT_BINARY_DIR}/nq-qml-scan)
    target_link_libraries(plasmashell PRIVATE nq_qml_imports)
endif()
# This project's KPluginFactory plugins (package structures, containment actions, the desktop:/ worker),
# registered with KCoreAddons by hand (kcoreaddons_target_static_plugins takes one TARGETS value only).
function(_nq_plugins dir out)
    set(found ${${out}})
    get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t IN LISTS targets)
        get_target_property(ns ${t} PLUGIN_INSTALL_NAMESPACE)
        if(ns)
            list(APPEND found ${t})
        endif()
    endforeach()
    get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(d IN LISTS subdirs)
        _nq_plugins(${d} found)
    endforeach()
    set(${out} ${found} PARENT_SCOPE)
endfunction()
set(_nq_own)
_nq_plugins(${CMAKE_CURRENT_SOURCE_DIR} _nq_own)
set(_nq_cpp "// plasma-workspace-end.cmake: this project's static plugins\n#include <QPluginLoader>\n#include <QString>\n#include <QtPlugin>\nextern void kRegisterStaticPluginFunction(const QString &, const QString &, QStaticPlugin);\n")
set(_nq_calls "")
foreach(_t IN LISTS _nq_own)
    get_target_property(_ns ${_t} PLUGIN_INSTALL_NAMESPACE)
    string(REPLACE "." "_" _f ${_t}_factory)
    string(REPLACE "-" "_" _f ${_f})
    # (registered by the plugin's id: its file name, the target's OUTPUT_NAME when it has one. The containment
    # actions' targets are plasma_containmentactions_contextmenu and the like, their files org.kde.contextmenu:
    # KPluginMetaData::findPluginById, by which libplasma looks for them, asks for the file name)
    get_target_property(_id ${_t} OUTPUT_NAME)
    if(NOT _id)
        set(_id ${_t})
    endif()
    string(APPEND _nq_cpp "extern const QStaticPlugin qt_static_plugin_${_f}();\n")
    string(APPEND _nq_calls "        kRegisterStaticPluginFunction(QStringLiteral(\"${_id}\"), QStringLiteral(\"${_ns}\"), qt_static_plugin_${_f}());\n")
    if(_ns STREQUAL "kf6/kio")
        string(APPEND _nq_calls "        qRegisterStaticPluginFunction(qt_static_plugin_${_f}());\n")
    endif()
    target_link_libraries(plasmashell PRIVATE ${_t})
endforeach()
string(APPEND _nq_cpp "namespace {\nstruct NqOwnPlugins {\n    NqOwnPlugins() {\n${_nq_calls}    }\n} nqOwnPlugins;\n}\n")
# Old-style QML plugins of this project (nq_old_qml_plugin): linked and imported.
get_property(_nq_old GLOBAL PROPERTY NQ_OLD_QML_PLUGINS)
foreach(_p IN LISTS _nq_old)
    string(REPLACE ":" ";" _p ${_p})
    list(GET _p 0 _t)
    list(GET _p 1 _c)
    if(TARGET ${_t})
        target_link_libraries(plasmashell PRIVATE ${_t} ${_t}_resources_99)
        string(APPEND _nq_cpp "Q_IMPORT_PLUGIN(${_c})\n")
    endif()
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/nq_own_plugins.cpp "${_nq_cpp}")
target_sources(plasmashell PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/nq_own_plugins.cpp)
# A stand-in for KPipeWire's QML module (native/qml-stubs/pipewire: no PipeWire for a static program), which
# the task manager's window thumbnails import: the import works, and no thumbnail comes.
qt_add_resources(plasmashell nq_pipewire_stub PREFIX /qt/qml/org/kde/pipewire BASE ${NQ_CMAKE_DIR}/qml-stubs/pipewire
    OPTIONS --no-zstd FILES ${NQ_CMAKE_DIR}/qml-stubs/pipewire/qmldir ${NQ_CMAKE_DIR}/qml-stubs/pipewire/PipeWireSourceItem.qml)
# Stand-ins for two more modules Debian's widgets import: KPipeWire's org.kde.pipewire.monitor (the camera
# indicator) and plasma-desktop's org.kde.plasma.private.kcm_keyboard (the keyboard layout widget's flags).
qt_add_resources(plasmashell nq_pipewire_monitor_stub PREFIX /qt/qml/org/kde/pipewire/monitor BASE ${NQ_CMAKE_DIR}/qml-stubs/pipewire-monitor
    OPTIONS --no-zstd FILES ${NQ_CMAKE_DIR}/qml-stubs/pipewire-monitor/qmldir ${NQ_CMAKE_DIR}/qml-stubs/pipewire-monitor/MediaMonitor.qml
    ${NQ_CMAKE_DIR}/qml-stubs/pipewire-monitor/MediaRole.qml ${NQ_CMAKE_DIR}/qml-stubs/pipewire-monitor/NodeState.qml)
qt_add_resources(plasmashell nq_kcm_keyboard_stub PREFIX /qt/qml/org/kde/plasma/private/kcm_keyboard BASE ${NQ_CMAKE_DIR}/qml-stubs/kcm_keyboard
    OPTIONS --no-zstd FILES ${NQ_CMAKE_DIR}/qml-stubs/kcm_keyboard/qmldir ${NQ_CMAKE_DIR}/qml-stubs/kcm_keyboard/Flags.qml)
