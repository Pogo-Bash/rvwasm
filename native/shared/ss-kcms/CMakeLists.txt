# The System Settings modules (KCMs) of plasma-workspace 6.3.6 and plasma-desktop 6.3.6 that need only the shared
# Qt/KF6 set, as a project of their own (their top levels want X11, libkworkspace, kwin, libplasma ...). Their own CMake
# files are used as they are. Configure with -DPW_SRC=.../plasma-workspace -DPD_SRC=.../plasma-desktop.
# No X11: HAVE_X11 0 (krdb is rebuilt here without the X11 targets it names).
cmake_minimum_required(VERSION 3.16)
project(ss-kcms VERSION 6.3.6)
set(PROJECT_VERSION_MAJOR 6)
set(PROJECT_VERSION_MINOR 3)
set(PROJECT_VERSION_PATCH 6)
set(QT_MIN_VERSION "6.7.0")
set(KF6_MIN_VERSION "6.10.0")
set(KDE_COMPILERSETTINGS_LEVEL "5.82")
find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQtDeclareLoggingCategory)
include(ECMQmlModule)
include(ECMQtDeclareLoggingCategory)
include(ECMGenerateExportHeader)
include(ECMInstallIcons)
include(GenerateExportHeader)
include(FeatureSummary)
include(CMakePackageConfigHelpers)
find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Gui Widgets Quick Qml DBus Svg Xml)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS Parts CoreAddons Config ConfigWidgets I18n KIO KCMUtils GuiAddons
    WindowSystem Archive XmlGui IconThemes NewStuff Package Service DBusAddons ColorScheme Auth Solid Sonnet NotifyConfig GlobalAccel ItemModels Runner Svg Notifications)
add_definitions(-DQT_NO_URL_CAST_FROM_STRING -DQT_USE_QSTRINGBUILDER)
set(HAVE_X11 0)
set(HAVE_XCURSOR 0)
set(HAVE_XFIXES 0)
set(PLASMA_RELATIVE_DATA_INSTALL_DIR "plasma")
configure_file(${PW_SRC}/config-workspace.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-workspace.h)
configure_file(${PW_SRC}/config-X11.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-X11.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
set(KDED_DBUS_INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dbus/org.kde.kded6.xml)

# krdb: the Qt/GTK settings written for X11 clients; its X11 half is off (HAVE_X11 0)
add_library(krdb SHARED ${PW_SRC}/kcms/krdb/krdb.cpp)
generate_export_header(krdb)
target_include_directories(krdb PUBLIC ${PW_SRC}/kcms/krdb ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(krdb PRIVATE Qt::Widgets Qt::DBus KF6::CoreAddons KF6::DBusAddons KF6::GuiAddons KF6::I18n KF6::WindowSystem KF6::ColorScheme Qt::GuiPrivate)
install(TARGETS krdb ${KDE_INSTALL_TARGETS_DEFAULT_ARGS} LIBRARY NAMELINK_SKIP)

# Fonts: kfontinst's library (fontconfig: the static one of the sysroot) and the module; its preview engine (FcEngine:
# Xft on an X server) is replaced by FcEngineStub.cpp (Qt drawing), kxftconfig's QX11Info by Qt's screen DPI (nqshared patch).
find_package(Fontconfig REQUIRED)
add_definitions(-DHAVE_FONTCONFIG=1)
set(KFI ${PW_SRC}/kcms/kfontinst)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/kfontinst-lib)
configure_file(${KFI}/lib/config-paths.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/kfontinst-lib/config-paths.h)
add_library(kfontinst SHARED ${KFI}/lib/Misc.cpp ${KFI}/lib/Fc.cpp ${KFI}/lib/Family.cpp ${KFI}/lib/Style.cpp ${KFI}/lib/File.cpp ${KFI}/lib/WritingSystems.cpp)
target_include_directories(kfontinst PUBLIC ${KFI}/lib ${CMAKE_CURRENT_BINARY_DIR}/kfontinst-lib ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kfontinst PUBLIC KF6::I18n KF6::KIOCore Fontconfig::Fontconfig PRIVATE Qt::Xml Qt::Gui)
generate_export_header(kfontinst)
install(TARGETS kfontinst ${KDE_INSTALL_TARGETS_DEFAULT_ARGS} LIBRARY NAMELINK_SKIP)
include_directories(${KFI} ${KFI}/lib ${CMAKE_CURRENT_BINARY_DIR}/kfontinst-lib)
set(NQS_FCENGINE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/FcEngineStub.cpp)

add_subdirectory(${PW_SRC}/kcms/colors colors)
add_subdirectory(${PW_SRC}/kcms/fonts fonts)
add_subdirectory(${PW_SRC}/kcms/icons icons)
add_subdirectory(${PW_SRC}/kcms/style style)
add_subdirectory(${PD_SRC}/kcms/workspaceoptions workspaceoptions)

# Global Theme (look and feel): the module and its manager. PlasmaAutostart is a small static library of plasma-workspace; libkworkspace
# is only named in the link line (nothing in the module calls it), so an empty interface target stands in (defined after colors: its optional programs test for this target). The look-and-feel
# packages (Breeze, Breeze Dark, Breeze Twilight) are data: installed here as plasma_install_package would.
add_subdirectory(${PW_SRC}/startkde/plasmaautostart plasmaautostart)
# libkworkspace itself (logout, display manager, output order: needs systemd, X11, kwin) is not built; the one small part the autostart module
# uses, AutostartScriptDesktopFile, is, as a static library under its name
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/kworkspace_export.h "#pragma once\n#define KWORKSPACE_EXPORT __attribute__((visibility(\"default\")))\n")
add_library(nqs_kworkspace STATIC ${PW_SRC}/libkworkspace/autostartscriptdesktopfile.cpp)
set_property(TARGET nqs_kworkspace PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(nqs_kworkspace PUBLIC ${PW_SRC}/libkworkspace ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(nqs_kworkspace PUBLIC Qt::Core KF6::ConfigCore)
add_library(PW::KWorkspace ALIAS nqs_kworkspace)
add_subdirectory(${PW_SRC}/kcms/lookandfeel lookandfeel)
foreach(lnf breeze breezedark breezetwilight)
    install(DIRECTORY ${PW_SRC}/lookandfeel/org.kde.${lnf}/ DESTINATION ${KDE_INSTALL_DATADIR}/plasma/look-and-feel/org.kde.${lnf}.desktop PATTERN Messages.sh EXCLUDE)
endforeach()

# The rest of the modules that work with what this machine runs (the manual test sheets has the table).
set(CMAKE_AUTOMOC ON)
find_package(ICU REQUIRED COMPONENTS i18n uc)
add_library(X11::X11 INTERFACE IMPORTED)   # named in link lines only (kcm_access: its X11 code is under HAVE_X11)
add_subdirectory(${PW_SRC}/kcms/autostart autostart)
add_subdirectory(${PW_SRC}/kcms/region_language region_language)
add_subdirectory(${PD_SRC}/kcms/componentchooser componentchooser)
add_subdirectory(${PD_SRC}/kcms/desktoppaths desktoppaths)
add_subdirectory(${PD_SRC}/kcms/qtquicksettings qtquicksettings)
add_subdirectory(${PD_SRC}/kcms/solid_actions solid_actions)
add_subdirectory(${PD_SRC}/kcms/spellchecking spellchecking)
add_subdirectory(${PD_SRC}/kcms/dateandtime dateandtime)
add_subdirectory(${PD_SRC}/kcms/access access)
add_definitions(-DTRANSLATION_DOMAIN=\"kcm_plasmasearch\")
add_subdirectory(${PD_SRC}/kcms/runners/krunner krunner)
add_subdirectory(${PD_SRC}/kcms/runners/plasmasearch plasmasearch)
install(FILES ${PD_SRC}/kcms/runners/krunner.knsrc DESTINATION ${KDE_INSTALL_KNSRCDIR})

# Notifications: libnotificationmanager (its KScreen header is not used; Plasma::Plasma only named in the link line: nqshared patch) and the module;
# libcanberra is a stand-in header (stubs/canberra.h).
add_library(Canberra::Canberra INTERFACE IMPORTED)
target_include_directories(Canberra::Canberra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
add_subdirectory(${PW_SRC}/libnotificationmanager libnotificationmanager)
set_target_properties(notificationmanager PROPERTIES CXX_STANDARD 20)   # std::ranges
add_subdirectory(${PW_SRC}/kcms/notifications notifications)

# File Associations (kde-cli-tools)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/filetypes filetypes)

# Desktop Theme: needs libPlasma (shared, built from libplasma 6.3.6 by build-kf-shared.sh; the Plasma library only: PlasmaQuick needs X11-only KWindowInfo)
add_library(Plasma::Plasma SHARED IMPORTED)
set_target_properties(Plasma::Plasma PROPERTIES IMPORTED_LOCATION ${CMAKE_STAGING_PREFIX}/lib/libPlasma.so INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_STAGING_PREFIX}/include;${CMAKE_STAGING_PREFIX}/include/Plasma")
add_subdirectory(${PW_SRC}/kcms/desktoptheme desktoptheme)
add_subdirectory(${PW_SRC}/kcms/soundtheme soundtheme)

# The package structure of look-and-feel packages (plasma-workspace/shell/packageplugins/lookandfeel: Global Theme lists packages of this type)
kcoreaddons_add_plugin(plasma_lookandfeel SOURCES ${PW_SRC}/shell/packageplugins/lookandfeel/lookandfeel.cpp INSTALL_NAMESPACE "kf6/packagestructure")
target_link_libraries(plasma_lookandfeel KF6::Package)
