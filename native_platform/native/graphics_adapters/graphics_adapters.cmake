# Guarded, source-identity-checked graphics library replacements. This module
# deliberately does not depend on a particular executable's generated PCH.
set(XN_GRAPHICS_ADAPTER_ROOT "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(XN_GRAPHICS_HOOK_EMITTER
  "${CMAKE_CURRENT_LIST_DIR}/../../tools/native_graphics/emit_hooks.py" ABSOLUTE)

function(xn_attach_graphics_adapters target title generated_module)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(manifest "${XN_GRAPHICS_ADAPTER_ROOT}/${title}.toml")
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${target}_graphics_adapters")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${manifest}" "${XN_GRAPHICS_HOOK_EMITTER}")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${XN_GRAPHICS_HOOK_EMITTER}"
    --generated "${generated_module}" --manifest "${manifest}" --output "${output}"
    COMMAND_ERROR_IS_FATAL ANY)
  target_sources(${target} PRIVATE "${output}/native_graphics_hooks.cpp")
  target_include_directories(${target} PRIVATE "${XN_GRAPHICS_ADAPTER_ROOT}")
endfunction()
