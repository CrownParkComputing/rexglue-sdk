# Native migration verification harness: snap_replay (CLI) and its identity
# test. Buildable two ways:
#   * inside the SDK tree (REXGLUE_BUILD_TESTS=ON) against rex::runtime
#   * standalone, e.g. for a quick host check without configuring the SDK:
#       cmake -S SDK/tests/native -B build-native && cmake --build build-native
#     which compiles src/system/snapshot.cpp straight in (it depends only on
#     <rex/ppc/context.h>, simde, xxHash and toml++, all in the tree).
cmake_minimum_required(VERSION 3.20)

if(NOT TARGET rexruntime)
    project(rex_native_tests CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    get_filename_component(REX_NATIVE_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    add_library(snap_replay_runtime STATIC
        ${REX_NATIVE_SDK}/src/system/snapshot.cpp
        ${REX_NATIVE_SDK}/src/system/tee.cpp)
    target_include_directories(snap_replay_runtime PUBLIC
        ${REX_NATIVE_SDK}/include
        ${REX_NATIVE_SDK}/thirdparty/simde
        ${REX_NATIVE_SDK}/thirdparty/simde/simde
        ${REX_NATIVE_SDK}/thirdparty/xxHash
        ${REX_NATIVE_SDK}/thirdparty/tomlplusplus/include)
    set(REX_NATIVE_LINK snap_replay_runtime)
else()
    set(REX_NATIVE_LINK rexruntime tomlplusplus)
endif()

add_library(snap_replay_lib STATIC snap_replay.cpp)
target_include_directories(snap_replay_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(snap_replay_lib PUBLIC ${REX_NATIVE_LINK})
target_compile_options(snap_replay_lib PRIVATE -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING)

add_executable(snap_replay snap_replay_main.cpp)
target_link_libraries(snap_replay PRIVATE snap_replay_lib ${CMAKE_DL_LIBS})

add_executable(snap_replay_test snap_replay_test.cpp)
target_link_libraries(snap_replay_test PRIVATE snap_replay_lib)

add_executable(tee_test tee_test.cpp)
target_link_libraries(tee_test PRIVATE ${REX_NATIVE_LINK})

enable_testing()
add_test(NAME snap_replay_identity COMMAND snap_replay_test)
add_test(NAME tee_channel COMMAND tee_test)
