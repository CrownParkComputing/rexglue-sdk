# Optional source-only package. Including this module never builds or replaces
# an SDK runtime/GPU library. Normal SDK installation leaves it disabled.
option(REXGLUE_INSTALL_NATIVE_GRAPHICS
  "Install experimental audited native graphics hooks and verification tools" OFF)

if(REXGLUE_INSTALL_NATIVE_GRAPHICS)
  get_filename_component(_rexglue_native_graphics_root
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  install(DIRECTORY "${_rexglue_native_graphics_root}/include/rex/native_graphics"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/rex"
    COMPONENT NativeGraphics FILES_MATCHING PATTERN "*.h")
  install(FILES
    "${_rexglue_native_graphics_root}/cmake/rexglue_native_graphics.cmake"
    "${_rexglue_native_graphics_root}/cmake/rexglue_native_target.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rexglue"
    COMPONENT NativeGraphics)
  install(FILES
    "${_rexglue_native_graphics_root}/tools/native_graphics/emit_hooks.py"
    "${_rexglue_native_graphics_root}/tools/native_graphics/verify_hooks.py"
    "${_rexglue_native_graphics_root}/tools/native_graphics/oracle_probe.cpp"
    "${_rexglue_native_graphics_root}/tools/native_graphics/test_emit_hooks.py"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rexglue/native_graphics"
    COMPONENT NativeGraphics)
  install(FILES "${_rexglue_native_graphics_root}/docs/NATIVE_GRAPHICS.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/rexglue"
    COMPONENT NativeGraphics)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/rexglue_native_backend_install.cmake")
