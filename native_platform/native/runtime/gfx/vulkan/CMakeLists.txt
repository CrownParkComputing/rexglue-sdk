# xn_renderer_vulkan: VulkanRenderer (IRenderer) on SDL3 + Vulkan 1.3.
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
find_program(XN_GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(XN_HOST_UI_HEADERS)
foreach(shader host_ui.vert host_ui.frag host_sharpen.frag)
  string(REPLACE "." "_" shader_name "${shader}")
  set(header "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.h")
  add_custom_command(OUTPUT "${header}"
    COMMAND "${XN_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
      --vn "xn_${shader_name}" -o "${header}" "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${shader}" VERBATIM)
  list(APPEND XN_HOST_UI_HEADERS "${header}")
endforeach()
find_library(XN_XXHASH_LIB NAMES xxhash REQUIRED)
add_library(xn_render_trace STATIC ../render_trace.cpp)
target_include_directories(xn_render_trace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(xn_render_trace PRIVATE ${XN_XXHASH_LIB})
find_path(XN_TOMLPP_INCLUDE_DIR toml++/toml.hpp)
if(NOT XN_TOMLPP_INCLUDE_DIR)
  message(FATAL_ERROR "toml++ (toml++/toml.hpp) not found")
endif()

add_library(xn_renderer_vulkan STATIC
  device_layout.cpp
  mirror_guest_range.cpp
  gpu_work_profile.cpp
  vulkan_renderer.cpp
  vulkan_draw.cpp
  render_capture.cpp
  pipeline_warmup.cpp
  host_ui.cpp
  ${XN_HOST_UI_HEADERS}
)
add_library(xn::renderer_vulkan ALIAS xn_renderer_vulkan)
target_include_directories(xn_renderer_vulkan
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..
  PRIVATE ${XN_TOMLPP_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(xn_renderer_vulkan
  PUBLIC xn::shader_translator xn_memory_watch xn_render_trace Vulkan::Vulkan SDL3::SDL3
  PRIVATE ${XN_XXHASH_LIB}
)
target_compile_options(xn_renderer_vulkan PRIVATE -Wall -Wno-unused-function)
option(XN_RENDERER_LINE_INFO "Keep native renderer source line tables for CPU profiling" OFF)
if(XN_RENDERER_LINE_INFO)
  target_compile_options(xn_renderer_vulkan PRIVATE -gline-tables-only)
endif()
