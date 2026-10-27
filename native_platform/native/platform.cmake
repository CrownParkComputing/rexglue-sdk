# Shared native runtime, Vulkan renderer and Nuklear menu for DVD title ports.
# Include from a title's standalone native CMake project.
set(XN_PLATFORM_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(XN_THIRDPARTY "${XN_PLATFORM_ROOT}/thirdparty")
set(XN_BUILD_TESTS OFF)
find_package(SDL3 REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_subdirectory("${XN_PLATFORM_ROOT}/runtime" "${CMAKE_CURRENT_BINARY_DIR}/runtime")
add_subdirectory("${XN_PLATFORM_ROOT}/runtime/audio" "${CMAKE_CURRENT_BINARY_DIR}/runtime/audio")
add_subdirectory("${XN_PLATFORM_ROOT}/runtime/gfx/shader" "${CMAKE_CURRENT_BINARY_DIR}/runtime/gfx/shader")
add_subdirectory("${XN_PLATFORM_ROOT}/runtime/gfx/vulkan" "${CMAKE_CURRENT_BINARY_DIR}/runtime/gfx/vulkan")
add_library(xn_native_options STATIC
    "${XN_PLATFORM_ROOT}/native_options.cpp"
    "${XN_PLATFORM_ROOT}/native_settings.cpp"
    "${XN_PLATFORM_ROOT}/native_disc2.cpp"
    "${XN_PLATFORM_ROOT}/forza4_setup.cpp")
add_library(forza4_native_options ALIAS xn_native_options)
target_include_directories(xn_native_options
    PUBLIC "${XN_PLATFORM_ROOT}"
    PRIVATE "${XN_PLATFORM_ROOT}/../thirdparty/nuklear")
target_link_libraries(xn_native_options PUBLIC xn_runtime xn_renderer_vulkan xn_audio_imports)
target_compile_definitions(xn_native_options PRIVATE
    XN_NATIVE_FONT="${XN_PLATFORM_ROOT}/../resources/fonts/LiberationSans-Regular.ttf")

option(XN_BUILD_RENDER_TOOLS "Build shared renderer capture/replay tools and CPU probes" OFF)
if(XN_BUILD_RENDER_TOOLS)
  include("${XN_PLATFORM_ROOT}/render_tools.cmake")
endif()
