# xn_shader_translator: the Xenos microcode -> SPIR-V translator vendored from
# ReXGlue (itself from Xenia), with tiny shims replacing rexruntime's cvar,
# logging, assert, memory and filesystem services. Links glslang's SPIRV
# builder (pinned source snapshot) and nothing from rexruntime.
find_package(fmt REQUIRED)
find_package(Vulkan REQUIRED)

# glslang's SPIRV builder (SPIRV/SpvBuilder.h) is an internal header that the
# system glslang package does not install, so glslang is built from source.
# Default: the pinned snapshot in this repository. XN_GLSLANG_SOURCE_DIR
# can select a compatible external tree instead.
set(XN_GLSLANG_SOURCE_DIR
  "${XN_THIRDPARTY}/glslang"
  CACHE PATH "glslang source tree providing SPIRV/SpvBuilder.h")
if(NOT EXISTS ${XN_GLSLANG_SOURCE_DIR}/SPIRV/SpvBuilder.h)
  message(FATAL_ERROR "XN_GLSLANG_SOURCE_DIR (${XN_GLSLANG_SOURCE_DIR}) has no SPIRV/SpvBuilder.h")
endif()
if(NOT TARGET SPIRV)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  # SpirvBuilder subclasses spv::Builder and the translator dynamic_casts it.
  set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
  set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  add_subdirectory(${XN_GLSLANG_SOURCE_DIR} ${CMAKE_BINARY_DIR}/thirdparty/glslang EXCLUDE_FROM_ALL)
endif()

set(XN_SHADER_VENDORED
  # Register / microcode model.
  src/register_file.cpp
  src/registers.cpp
  src/ucode.cpp
  # Shader analysis, ucode disassembly and the translator base.
  src/shader.cpp
  src/translator.cpp
  src/translator_disasm.cpp
  # Viewport / scissor / colour-mask helpers + D3D10 sample positions.
  src/draw_util.cpp
  # xenos.h out-of-line helpers (float20e4) and rex::string::StringBuffer.
  src/xenos.cpp
  src/string_buffer.cpp
  # Microcode -> SPIR-V.
  src/spirv_builder.cpp
  src/spirv.cpp
  src/spirv_translator.cpp
  src/native_gpu_constants.cpp
  src/spirv_translator_alu.cpp
  src/spirv_translator_fetch.cpp
  src/spirv_translator_memexport.cpp
  src/spirv_translator_rb.cpp
)
set(XN_SHADER_SHIMS
  src/xn_flags.cpp
  src/xn_math.cpp
)
add_library(xn_shader_translator STATIC ${XN_SHADER_VENDORED} ${XN_SHADER_SHIMS})
add_library(xn::shader_translator ALIAS xn_shader_translator)
target_include_directories(xn_shader_translator PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/shim
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${XN_GLSLANG_SOURCE_DIR}
)
target_compile_definitions(xn_shader_translator PUBLIC REX_SHADER_TRANSLATOR_STANDALONE=1)
target_compile_options(xn_shader_translator PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable)
set_target_properties(xn_shader_translator PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(xn_shader_translator
  PUBLIC fmt::fmt Vulkan::Headers
  PRIVATE SPIRV glslang
)
