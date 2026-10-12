cmake_minimum_required(VERSION 3.21)
project(qcore-test CXX)
find_package(Qt6 REQUIRED COMPONENTS Core)
qt_add_executable(qcore-test main.cpp)
target_link_libraries(qcore-test PRIVATE Qt6::Core)
# NQ_THROW_TRACE=1 at run time names every thrown C++ exception (crt/nqcrt.c).
target_link_options(qcore-test PRIVATE -Wl,--wrap=__cxa_throw -Wl,--wrap=_Unwind_RaiseException)
