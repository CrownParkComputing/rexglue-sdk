# The standalone native platform is a separate optional source component.
option(REXGLUE_INSTALL_NATIVE_BACKEND
  "Install the audited standalone native runtime/Vulkan/audio/menu sources" OFF)

if(REXGLUE_INSTALL_NATIVE_BACKEND)
  get_filename_component(_rexglue_native_backend_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  set(_rexglue_native_backend_bundle "${_rexglue_native_backend_root}/native_platform")
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${_rexglue_native_backend_root}/tools/native_graphics/export_native_platform.py"
    --verify-bundle "${_rexglue_native_backend_bundle}"
    COMMAND_ERROR_IS_FATAL ANY)
  file(READ "${_rexglue_native_backend_bundle}/manifest.json" _rexglue_native_backend_manifest)
  string(JSON _rexglue_native_backend_count LENGTH "${_rexglue_native_backend_manifest}" files)
  math(EXPR _rexglue_native_backend_last "${_rexglue_native_backend_count} - 1")
  foreach(index RANGE 0 ${_rexglue_native_backend_last})
    string(JSON path GET "${_rexglue_native_backend_manifest}" files ${index} path)
    string(JSON mode GET "${_rexglue_native_backend_manifest}" files ${index} mode)
    get_filename_component(directory "${path}" DIRECTORY)
    if(mode EQUAL 493)
      install(PROGRAMS "${_rexglue_native_backend_bundle}/${path}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/rexglue/native_platform/${directory}"
        COMPONENT NativeBackend)
    else()
      install(FILES "${_rexglue_native_backend_bundle}/${path}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/rexglue/native_platform/${directory}"
        COMPONENT NativeBackend)
    endif()
  endforeach()
  install(FILES "${_rexglue_native_backend_bundle}/manifest.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rexglue/native_platform"
    COMPONENT NativeBackend)
  install(FILES "${_rexglue_native_backend_root}/cmake/rexglue_native_target.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rexglue"
    COMPONENT NativeBackend)
  install(FILES "${_rexglue_native_backend_root}/tools/native_graphics/export_native_platform.py"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rexglue/native_graphics"
    COMPONENT NativeBackend)
  install(FILES "${_rexglue_native_backend_root}/docs/NATIVE_GRAPHICS.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/rexglue"
    COMPONENT NativeBackend)
endif()
