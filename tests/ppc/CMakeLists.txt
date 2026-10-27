# PowerPC Instruction Test Suite
# Compiles PPC assembly tests and generates Catch2 test cases

include(ppc_test_pipeline)

# Output directories
set(PPC_OBJ_DIR ${CMAKE_BINARY_DIR}/tests/ppc/obj)
set(PPC_BIN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/bin)
set(PPC_GEN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/generated)

file(MAKE_DIRECTORY ${PPC_OBJ_DIR})
file(MAKE_DIRECTORY ${PPC_BIN_DIR})
file(MAKE_DIRECTORY ${PPC_GEN_DIR})

# Collect and build all assembly test files
file(GLOB ASM_FILES ${CMAKE_CURRENT_SOURCE_DIR}/asm/*.s)
list(LENGTH ASM_FILES NUM_TESTS)

if(NUM_TESTS EQUAL 0)
    message(WARNING "No test files found in ${CMAKE_CURRENT_SOURCE_DIR}/asm/")
    return()
endif()

message(STATUS "Found ${NUM_TESTS} PPC instruction test files")

foreach(ASM_FILE ${ASM_FILES})
    ppc_add_test_binary(${ASM_FILE} ${PPC_OBJ_DIR} ${PPC_BIN_DIR})
endforeach()

get_property(TEST_BINS GLOBAL PROPERTY PPC_TEST_BINS)
get_property(TEST_MAPS GLOBAL PROPERTY PPC_TEST_MAPS)

# Codegen flag variants (A/B protocol, docs/native/CODEGEN_FLAGS.md).
#
# Every entry builds one ppc_tests executable from the same assembly set, each
# regenerated with the named manifest flags. "default" is the shipped codegen;
# a variant is written as flags joined by "+", e.g.
#   -DREXGLUE_PPC_TEST_VARIANTS="default;non_volatile_as_local;nonvolatile_memory;non_volatile_as_local+nonvolatile_memory"
# The default entry keeps its historical name ppc_tests; a variant is
# ppc_tests_<flags with + replaced by _>. tools/ppc_oracle/ab_flags.sh drives
# the whole matrix and diffs the per-test results.
set(REXGLUE_PPC_TEST_VARIANTS "default" CACHE STRING
    "Codegen flag sets to build the PPC instruction tests with (';'-separated, flags joined by '+')")

function(_ppc_add_test_variant VARIANT)
    if(VARIANT STREQUAL "default" OR VARIANT STREQUAL "")
        set(TARGET ppc_tests)
        set(GEN_DIR ${PPC_GEN_DIR})
        set(FLAG_ARGS "")
        set(PREFIX "ppc.")
    else()
        string(REPLACE "+" "_" SUFFIX "${VARIANT}")
        set(TARGET ppc_tests_${SUFFIX})
        set(GEN_DIR ${PPC_GEN_DIR}/${SUFFIX})
        string(REPLACE "+" ";" FLAG_LIST "${VARIANT}")
        set(FLAG_ARGS "")
        foreach(FLAG ${FLAG_LIST})
            list(APPEND FLAG_ARGS --codegen-flag ${FLAG})
        endforeach()
        set(PREFIX "ppc.${SUFFIX}.")
        file(MAKE_DIRECTORY ${GEN_DIR})
    endif()

    # Generate Catch2 test code from recompiled binaries
    add_custom_command(
        OUTPUT ${GEN_DIR}/ppc_test_functions.cpp
               ${GEN_DIR}/ppc_test_cases.cpp
               ${GEN_DIR}/ppc_test_decls.h
        COMMAND $<TARGET_FILE:rexglue> recompile-tests
                --output=${GEN_DIR}
                --bin-dir=${PPC_BIN_DIR}
                --asm-dir=${CMAKE_CURRENT_SOURCE_DIR}/asm
                ${FLAG_ARGS}
        DEPENDS ${TEST_BINS} ${TEST_MAPS} ${ASM_FILES} rexglue
        COMMENT "Generating Catch2 test code with rexglue (${VARIANT})"
        VERBATIM
    )

    add_executable(${TARGET}
        ${GEN_DIR}/ppc_test_functions.cpp
        ${GEN_DIR}/ppc_test_cases.cpp
    )

    set_source_files_properties(
        ${GEN_DIR}/ppc_test_functions.cpp
        ${GEN_DIR}/ppc_test_cases.cpp
        ${GEN_DIR}/ppc_test_decls.h
        PROPERTIES GENERATED TRUE
    )

    target_include_directories(${TARGET} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${GEN_DIR}
    )

    target_link_libraries(${TARGET} PRIVATE
        simde
        rexruntime
        fmt::fmt
        Catch2::Catch2WithMain
    )

    # Delay-load FidelityFX DLLs so the tests can run without them.
    if(WIN32)
        if(TARGET amd_fidelityfx_vk)
            target_link_options(${TARGET} PRIVATE
                "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:amd_fidelityfx_vk>")
        endif()
        if(TARGET amd_fidelityfx_dx12)
            target_link_options(${TARGET} PRIVATE
                "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:amd_fidelityfx_dx12>")
        endif()
        if(TARGET amd_fidelityfx_vk OR TARGET amd_fidelityfx_dx12)
            target_link_libraries(${TARGET} PRIVATE delayimp)
        endif()
    endif()

    set_target_properties(${TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
    )
    if(UNIX AND NOT APPLE)
        set_target_properties(${TARGET} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    target_compile_options(${TARGET} PRIVATE
        -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
        -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
        -msse4.1 -mssse3
    )

    include(Catch)
    catch_discover_tests(${TARGET}
        TEST_PREFIX "${PREFIX}"
        PROPERTIES
            LABELS "ppc;instructions;${VARIANT}"
            TIMEOUT 60
    )
endfunction()

foreach(VARIANT ${REXGLUE_PPC_TEST_VARIANTS})
    _ppc_add_test_variant("${VARIANT}")
endforeach()

message(STATUS "Configured PPC test suite with ${NUM_TESTS} test files")
