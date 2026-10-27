# Host test for rex::shader-translator: translate every dumped Xenos shader in
# a directory (the --dump_shaders layout) to SPIR-V and report failures.
#
#   cmake --build <build> --target shader_translator_test
#   <build>/bin/shader_translator_test [dir] [--features=all|min|both]
#                                      [--show=HASH ...] [--dump=<dir>] [-v]
add_executable(shader_translator_test shader_translator_test.cpp)
target_link_libraries(shader_translator_test PRIVATE
    rex::shader-translator
    xxHash::xxhash
)
target_compile_options(shader_translator_test PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
)
set_target_properties(shader_translator_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(shader_translator_test PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()
if(BUILD_TESTING)
    add_test(NAME shader_translator.pgr4_dump
        COMMAND shader_translator_test /home/jon/pgr4dump/shaders --features=both)
    set_tests_properties(shader_translator.pgr4_dump PROPERTIES LABELS "shader_translator")
endif()
