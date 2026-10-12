cmake_minimum_required(VERSION 3.21)
project(qquick-test CXX)
find_package(Qt6 REQUIRED COMPONENTS Gui Quick Qml WaylandClient)
qt_add_executable(qquick-test main.cpp)
qt_add_resources(qquick-test qml PREFIX / FILES main.qml)
target_link_libraries(qquick-test PRIVATE Qt6::Gui Qt6::Quick Qt6::Qml)
# Static Qt: the Wayland platform plugin, its shell integration and its EGL client buffer integration
# linked in; and the QML modules the program imports (QtQuick), found by qmlimportscanner.
qt_import_plugins(qquick-test INCLUDE Qt6::QWaylandIntegrationPlugin Qt6::QWaylandXdgShellIntegrationPlugin
	Qt6::QWaylandEglClientBufferPlugin)
qt_import_qml_plugins(qquick-test)
target_link_options(qquick-test PRIVATE -Wl,--wrap=__cxa_throw -Wl,--wrap=_Unwind_RaiseException -Wl,--wrap=malloc)
