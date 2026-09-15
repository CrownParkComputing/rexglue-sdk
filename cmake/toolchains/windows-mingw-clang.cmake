# Cross-compile for Windows x86-64 from Linux, with clang against the MinGW-w64
# sysroot.
#
# clang, not the MinGW g++: DEFINE_REX_FUNC declares each recompiled function as
# a weak alias of its __imp__ twin, and g++ drops those aliases, which surfaces
# as ~30,000 undefined symbols at link. clang keeps them - verified against this
# sysroot before this file was written.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TRIPLE x86_64-w64-mingw32)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${TRIPLE})
set(CMAKE_RC_COMPILER ${TRIPLE}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
