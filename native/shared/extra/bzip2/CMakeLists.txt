# bzip2 1.0.8 as a shared library (its own build is a Makefile): copied over the tarball's top level by build-kf-shared.sh.
cmake_minimum_required(VERSION 3.16)
project(bzip2 VERSION 1.0.8 LANGUAGES C)
add_library(bz2 SHARED blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c)
target_compile_definitions(bz2 PRIVATE _FILE_OFFSET_BITS=64)
target_compile_options(bz2 PRIVATE -fvisibility=default)
set_target_properties(bz2 PROPERTIES VERSION 1.0.8 SOVERSION 1.0)
install(TARGETS bz2 LIBRARY DESTINATION lib)
install(FILES bzlib.h DESTINATION include)
