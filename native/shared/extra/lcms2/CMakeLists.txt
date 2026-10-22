# Little CMS 2.16 as a shared library for the wasm dynamic linker (its own build is autotools or meson): copied over the tarball's top
# level by build-kf-shared.sh (native/shared/extra/NAME/CMakeLists.txt). Default symbol visibility (a C library here is hidden otherwise).
cmake_minimum_required(VERSION 3.16)
project(lcms2 VERSION 2.16 LANGUAGES C)
file(GLOB LCMS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_library(lcms2 SHARED ${LCMS_SRC})
target_include_directories(lcms2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lcms2 PRIVATE -fvisibility=default)
set_target_properties(lcms2 PROPERTIES VERSION 2.0.16 SOVERSION 2)
install(TARGETS lcms2 LIBRARY DESTINATION lib)
install(FILES include/lcms2.h include/lcms2_plugin.h DESTINATION include)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/lcms2.pc "prefix=/usr\nlibdir=${CMAKE_INSTALL_PREFIX}/lib\nincludedir=${CMAKE_INSTALL_PREFIX}/include\nName: lcms2\nDescription: LCMS Color Management Library\nVersion: 2.16\nLibs: -L\${libdir} -llcms2\nCflags: -I\${includedir}\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lcms2.pc DESTINATION lib/pkgconfig)
