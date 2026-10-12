cmake_minimum_required(VERSION 3.21)
project(launch-test CXX)
find_package(Qt6 REQUIRED COMPONENTS Core)
qt_add_executable(launch-test main.cpp)
target_link_libraries(launch-test PRIVATE Qt6::Core)
# NQ_THROW_TRACE=1 at run time names every thrown C++ exception (crt/nqcrt.c).
target_link_options(launch-test PRIVATE -Wl,--wrap=__cxa_throw -Wl,--wrap=_Unwind_RaiseException)
