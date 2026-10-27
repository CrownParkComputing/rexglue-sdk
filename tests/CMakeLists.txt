# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)

# Native migration harness (snap_replay, tee)
add_subdirectory(native)

# Full runtime I/O needs a process of its own (it owns a guest memory arena).
add_executable(runtime_io_tests runtime_io_test.cpp)
target_link_libraries(runtime_io_tests PRIVATE rexruntime)
set_target_properties(runtime_io_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME runtime.io_write_completion COMMAND runtime_io_tests)
set_tests_properties(runtime.io_write_completion PROPERTIES TIMEOUT 30)
