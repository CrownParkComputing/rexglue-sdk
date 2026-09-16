# rexiso - list or extract an Xbox 360 disc image (XDVDFS) with the runtime's
# own reader. A host tool: it ships beside the launcher so tools/import_content.sh
# can import a plain .iso the same way it imports a .zip.
add_executable(rexiso main.cpp)
rex_config_postfix(rexiso)
target_include_directories(rexiso PRIVATE ${REXGLUE_ROOT}/src)
# Both are OBJECT libraries: objects do not travel through another OBJECT
# library's link interface, so core is named as well as filesystem.
target_link_libraries(rexiso PRIVATE rex::filesystem rex::core)
if(WIN32)
    # rexcore's socket layer is winsock; the runtime DLL names ws2_32 itself.
    target_link_libraries(rexiso PRIVATE ws2_32)
endif()
