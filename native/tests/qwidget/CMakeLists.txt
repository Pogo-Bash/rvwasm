cmake_minimum_required(VERSION 3.21)
project(qwidget-test CXX)
find_package(Qt6 REQUIRED COMPONENTS Widgets WaylandClient)
qt_add_executable(qwidget-test main.cpp)
target_link_libraries(qwidget-test PRIVATE Qt6::Widgets)
# Static Qt: the Wayland platform plugin (and its shell integration) linked in.
# (One INCLUDE list: cmake_parse_arguments keeps only the last of two INCLUDE_BY_TYPE.)
qt_import_plugins(qwidget-test INCLUDE Qt6::QWaylandIntegrationPlugin Qt6::QWaylandXdgShellIntegrationPlugin)
# NQ_THROW_TRACE=1 at run time names every thrown C++ exception (crt/nqcrt.c).
target_link_options(qwidget-test PRIVATE -Wl,--wrap=__cxa_throw -Wl,--wrap=_Unwind_RaiseException -Wl,--wrap=malloc -Wl,--export=__cpp_exception)
