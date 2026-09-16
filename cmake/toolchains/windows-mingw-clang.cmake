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

# Package search is confined to the sysroot below, so an installed SDK that
# lives elsewhere (out/install/win-amd64) has to be named as a second root:
#   -DREXGLUE_FIND_ROOT_EXTRA=/path/to/out/install/win-amd64
# A -D cache entry is visible here; rexglue_DIR alone is re-rooted and lost.
set(REXGLUE_FIND_ROOT_EXTRA "" CACHE PATH "Extra CMAKE_FIND_ROOT_PATH entry (an installed SDK)")
set(CMAKE_FIND_ROOT_PATH /usr/${TRIPLE} ${REXGLUE_FIND_ROOT_EXTRA})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
