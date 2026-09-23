set(REXSYSTEM_SOURCES
    runtime.cpp
    embedded_metadata.cpp
    achievement_manager.cpp
    high_scores.cpp
    achievements.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    guest_path.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    guest_memory_watch.cpp
    import_trace.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    gpu_plugin_loader.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

# A shared runtime carries every kernel export for every title - 2,866 hooks -
# because it cannot know which one will load it. Built static instead, with
# function sections and --gc-sections on the consumer, the linker keeps only the
# kernel surface the title actually references and the port stops depending on
# librexruntime at all. Per-title rather than global: the GPU plugins are still
# dlopened and still need the shared build, so this is an option, not a switch.
option(REXGLUE_RUNTIME_STATIC "Build rexruntime as a static library" OFF)
if(REXGLUE_RUNTIME_STATIC)
    add_library(rexruntime STATIC ${REXSYSTEM_SOURCES})
    # Each function and variable in its own section, so --gc-sections can drop
    # the ones nothing reaches.
    target_compile_options(rexruntime PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-ffunction-sections;-fdata-sections>)
else()
    add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})
endif()
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)

if(REXGLUE_GPU_BUILTIN)
    target_compile_definitions(rexruntime PRIVATE REXGLUE_GPU_BUILTIN=1)
    target_link_libraries(rexruntime PUBLIC rexgpu-xenos)
endif()

set_target_properties(rexruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

rex_target_pch(rexruntime
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
        SDL3::SDL3
    PRIVATE
        aes128
        mspack
        xxHash::xxhash
)

if(REXGLUE_ENABLE_TRACY)
    # TARGET_NAME is what tells install(EXPORT) this text is a target to
    # namespace. Without it the plain name survives into the installed config
    # and consumers link a bare TracyClient they have no search path for.
    target_link_libraries(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE ws2_32)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid)
    target_compile_definitions(rexruntime PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MINGW)
        # The kernel hooks carry an explicit dllexport (see REX_FUNC_LINKAGE in
        # rex/ppc/func.h), because ld's auto-export skips every "__imp_" name.
        # Any explicit export switches ld out of auto-export mode entirely, so
        # ask for it back or the plugins lose the rest of the runtime's API.
        target_link_options(rexruntime PRIVATE -Wl,--export-all-symbols)
    endif()
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()
