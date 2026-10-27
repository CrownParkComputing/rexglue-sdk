# Optional, audited graphics-library replacements for ReXGlue generated code.
# In the SDK this file lives beside rexglueConfig.cmake. Source distributions
# may set REXGLUE_NATIVE_GRAPHICS_TOOL_DIR and _HEADER_DIR explicitly.
include_guard(GLOBAL)

function(rexglue_attach_native_graphics target)
  cmake_parse_arguments(NATIVE "" "MANIFEST;GENERATED" "" ${ARGN})
  if(NATIVE_UNPARSED_ARGUMENTS OR NOT NATIVE_MANIFEST OR NOT NATIVE_GENERATED)
    message(FATAL_ERROR "rexglue_attach_native_graphics(target MANIFEST metadata.toml GENERATED module_directory)")
  endif()
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  if(REXGLUE_NATIVE_GRAPHICS_TOOL_DIR)
    set(tool_dir "${REXGLUE_NATIVE_GRAPHICS_TOOL_DIR}")
  elseif(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/native_graphics/emit_hooks.py")
    get_filename_component(tool_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/native_graphics" ABSOLUTE)
  else()
    get_filename_component(tool_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../share/rexglue/native_graphics" ABSOLUTE)
  endif()
  if(REXGLUE_NATIVE_GRAPHICS_HEADER_DIR)
    set(header_dir "${REXGLUE_NATIVE_GRAPHICS_HEADER_DIR}")
  elseif(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include/rex/native_graphics/constant_setter.h")
    get_filename_component(header_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include/rex/native_graphics" ABSOLUTE)
  else()
    get_filename_component(header_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../include/rex/native_graphics" ABSOLUTE)
  endif()
  if(NOT EXISTS "${tool_dir}/emit_hooks.py" OR NOT EXISTS "${header_dir}/constant_setter.h")
    message(FATAL_ERROR "Native graphics tools/headers not found; install the experimental package or set REXGLUE_NATIVE_GRAPHICS_TOOL_DIR and REXGLUE_NATIVE_GRAPHICS_HEADER_DIR")
  endif()
  get_filename_component(manifest "${NATIVE_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  get_filename_component(generated "${NATIVE_GENERATED}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${target}_native_graphics")
  file(GLOB codegen_inputs CONFIGURE_DEPENDS "${generated}/*_recomp.*.cpp")
  if(NOT codegen_inputs)
    message(FATAL_ERROR "Generate the title module with ReXGlue before binding audited native graphics hooks")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${manifest}" "${tool_dir}/emit_hooks.py" ${codegen_inputs})
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${tool_dir}/emit_hooks.py"
    --generated "${generated}" --manifest "${manifest}" --output "${output}"
    COMMAND_ERROR_IS_FATAL ANY)
  # Direct executable sources matter: a STATIC archive's strong overrides may
  # not be extracted when a generated weak definition already satisfies a call.
  target_sources(${target} PRIVATE "${output}/native_graphics_hooks.cpp")
  target_include_directories(${target} PRIVATE "${header_dir}")
  target_compile_features(${target} PRIVATE cxx_std_23)
endfunction()
