cmake_minimum_required(VERSION 3.21)
project(knewmenu-test CXX)
find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(KF6KIO REQUIRED COMPONENTS Core Widgets FileWidgets)
qt_add_executable(knewmenu-test main.cpp)
target_link_libraries(knewmenu-test PRIVATE Qt6::Widgets KF6::KIOFileWidgets KF6::KIOWidgets KF6::KIOCore)
# Static Qt: the offscreen platform plugin linked in.
qt_import_plugins(knewmenu-test INCLUDE Qt6::QOffscreenIntegrationPlugin)
# NQ_THROW_TRACE=1 at run time names every thrown C++ exception (crt/nqcrt.c).
target_link_options(knewmenu-test PRIVATE -Wl,--wrap=__cxa_throw -Wl,--wrap=_Unwind_RaiseException)
