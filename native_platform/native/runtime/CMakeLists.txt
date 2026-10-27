# xn_loader: XEX decode only (AES + SHA1 + LZX). No guest memory, no kernel.
add_library(xn_loader STATIC
  loader/xex.cpp
  loader/xctd.cpp
  loader/title_metadata.cpp
  ${XN_THIRDPARTY}/crypto/rijndael-alg-fst.c
  ${XN_THIRDPARTY}/mspack/lzxd.c)
target_include_directories(xn_loader
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${XN_THIRDPARTY}/crypto ${XN_THIRDPARTY}/mspack)
target_compile_options(xn_loader PRIVATE -w)

add_library(xn_memory_watch STATIC memory/physical_write_watch.cpp)
target_include_directories(xn_memory_watch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# xn_runtime: the native Xbox 360 runtime (codegen contract, arena, kernel, gfx seam).
add_library(xn_runtime STATIC
  core/log.cpp
  core/clock.cpp
  ppc/dispatch.cpp
  memory/arena.cpp
  kernel/kernel.cpp
  kernel/modules.cpp
  kernel/fibers.cpp
  kernel/profile_store.cpp
  kernel/local_profile.cpp
  kernel/stats_store.cpp
  kernel/objects.cpp
  kernel/threads.cpp
  kernel/imports_threading.cpp
  kernel/imports_sync.cpp
  kernel/imports_memory.cpp
  kernel/imports_rtl.cpp
  kernel/imports_launch.cpp
  kernel/imports_crypt.cpp
  ${XN_THIRDPARTY}/crypto/sha256.cpp
  kernel/imports_io.cpp
  kernel/imports_content.cpp
  kernel/imports_ui.cpp
  kernel/imports_achievements.cpp
  kernel/imports_video.cpp
  kernel/imports_xam.cpp
  kernel/xmp.cpp
  kernel/input_script.cpp
  kernel/imports_misc.cpp
  kernel/imports_trace.cpp
  kernel/format.cpp
  gfx/command_processor.cpp
  gfx/frame_rate.cpp
  gfx/stub_renderer.cpp
  gfx/frame_recorder.cpp)
target_include_directories(xn_runtime
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ppc/include ${XN_THIRDPARTY})
find_library(XN_XXHASH_LIB NAMES xxhash REQUIRED)  # shader hashes as the oracle computes them
target_link_libraries(xn_runtime PUBLIC xn_loader xn_audio xn_memory_watch SDL3::SDL3 pthread ${XN_XXHASH_LIB})
find_package(SQLite3 REQUIRED)
target_link_libraries(xn_runtime PUBLIC SQLite::SQLite3)
target_compile_options(xn_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)
