# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/geometry_shader_builder.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

# Built into the title rather than dlopened when the runtime is static: a
# plugin binds the runtime's symbols from librexruntime.so, and a static build
# has no such library for it to bind to.
option(REXGLUE_GPU_BUILTIN "Link the Xenos GPU backend into the title" OFF)
if(REXGLUE_GPU_BUILTIN)
    add_library(rexgpu-xenos STATIC ${REXGPU_XENOS_SOURCES})
    target_compile_options(rexgpu-xenos PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-ffunction-sections;-fdata-sections>)
else()
    add_library(rexgpu-xenos SHARED ${REXGPU_XENOS_SOURCES})
endif()
add_library(rex::gpu-xenos ALIAS rexgpu-xenos)

set_target_properties(rexgpu-xenos PROPERTIES
    EXPORT_NAME gpu-xenos
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# The loader asks for "rexgpu-<name>.dll" on Windows (gpu_plugin_loader.cpp),
# but MinGW would name it librexgpu-<name>.dll. Drop the prefix so the two
# agree; MSVC already emits no prefix.
if(WIN32)
    set_target_properties(rexgpu-xenos PROPERTIES PREFIX "")
endif()

target_include_directories(rexgpu-xenos PRIVATE
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
)

target_compile_options(rexgpu-xenos PRIVATE -Wno-unused-variable)

rex_target_pch(rexgpu-xenos
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/thread/mutex.h>
    <rex/system/xmemory.h>
    <rex/graphics/xenos.h>
    <rex/graphics/registers.h>
    <rex/graphics/format/ucode.h>
    <rex/graphics/pipeline/shader/shader.h>
)

if(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Everything PRIVATE: the plugin has no consumers. Never link the OBJECT
# libraries (rexcore/rexui) here; rexruntime already contains those objects
# and relinking them would duplicate global state.
target_link_libraries(rexgpu-xenos PRIVATE
    rexruntime
    xxHash::xxhash
)

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgpu-xenos PRIVATE
        Vulkan::Headers
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexgpu-xenos PRIVATE
        d3d12
        dxgi
        dxguid
    )
endif()

#==============================================================================
# rexgpu-native: game-agnostic NATIVE GPU renderer plugin
#
# Selected at runtime with --gpu_plugin native (loads librexgpu-native.so).
# Shares the backend-agnostic PM4 command processor and Xenos base with
# rexgpu-xenos and replaces the backend draw/resolve/present seams with native
# GPU command translation - no EDRAM, tiling or resolve emulation. Independent
# of rexgpu-xenos: it links none of it and compiles its own copies of the
# shared sources.
#==============================================================================
if(REXGLUE_USE_VULKAN)
    set(REXGPU_NATIVE_SOURCES
        native/plugin_main.cpp
        native/graphics_system.cpp
        native/command_processor.cpp
        native/native_shared_memory.cpp
        native/texture_cache.cpp

        # Geometry-shader SPIR-V construction, split out of
        # vulkan/pipeline_cache.cpp so it can be compiled here without the rest
        # of VulkanPipelineCache.
        vulkan/geometry_shader_builder.cpp

        # Shared, game-agnostic PM4 + Xenos base.
        flags.cpp
        register_file.cpp
        registers.cpp
        pipeline/shader/shader.cpp
        format/ucode.cpp
        sampler_info.cpp
        graphics_system.cpp
        command_processor.cpp
        util/draw_extent_estimator.cpp
        util/draw.cpp
        packet_disassembler.cpp
        primitive_processor.cpp
        pipeline/render_target/cache.cpp
        shared_memory.cpp
        pipeline/texture/cache.cpp
        pipeline/shader/interpreter.cpp
        pipeline/shader/translator.cpp
        pipeline/shader/translator_disasm.cpp

        # Xenos microcode -> SPIR-V, reused verbatim: the native backend
        # translates guest shaders instead of emulating the Xenos ALU.
        pipeline/shader/spirv_builder.cpp
        pipeline/shader/spirv.cpp
        pipeline/shader/spirv_translator.cpp
        pipeline/shader/spirv_translator_alu.cpp
        pipeline/shader/spirv_translator_fetch.cpp
        pipeline/shader/spirv_translator_memexport.cpp
        pipeline/shader/spirv_translator_rb.cpp
    )

    add_library(rexgpu-native SHARED ${REXGPU_NATIVE_SOURCES})
    add_library(rex::gpu-native ALIAS rexgpu-native)

    # Explicit opt-in name for the direct Vulkan path while the replacement
    # translator is developed. This leaves the stable xenos default intact.
    add_library(rexgpu-transition SHARED ${REXGPU_NATIVE_SOURCES})
    add_library(rex::gpu-transition ALIAS rexgpu-transition)

    set_target_properties(rexgpu-native PROPERTIES
        EXPORT_NAME gpu-native
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    set_target_properties(rexgpu-transition PROPERTIES
        EXPORT_NAME gpu-transition
        OUTPUT_NAME rexgpu-transition
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    # The loader asks for "rexgpu-<name>.dll" on Windows (gpu_plugin_loader.cpp),
    # but MinGW would name it librexgpu-<name>.dll. Drop the prefix so the two
    # agree; MSVC already emits no prefix.
    if(WIN32)
        set_target_properties(rexgpu-native PROPERTIES PREFIX "")
        set_target_properties(rexgpu-transition PROPERTIES PREFIX "")
    endif()

    target_include_directories(rexgpu-native PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${REXGLUE_ROOT}/thirdparty/renderdoc
        ${REXGLUE_ROOT}
    )
    target_include_directories(rexgpu-transition PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${REXGLUE_ROOT}/thirdparty/renderdoc
        ${REXGLUE_ROOT}
    )

    target_compile_options(rexgpu-native PRIVATE -Wno-unused-variable)
    target_compile_options(rexgpu-transition PRIVATE
        -Wno-unused-variable
        -DREX_GPU_PLUGIN_NAME=\"transition\"
    )

    target_link_libraries(rexgpu-native PRIVATE
        rexruntime
        xxHash::xxhash
        # snappy was inherited from rexgpu-xenos's link list and is used by
        # neither plugin - no source in src/graphics references it. Keeping it
        # was what blocked this plugin from cross-compiling.
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        glslang::SPIRV
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
    target_link_libraries(rexgpu-transition PRIVATE
        rexruntime
        xxHash::xxhash
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        glslang::SPIRV
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
endif()

#==============================================================================
# rexgpu-stub: headless guest GPU plugin
#
# Selected at runtime with --gpu_plugin stub (loads librexgpu-stub.so).
# Reuses the backend-agnostic PM4 command processor and GraphicsSystem base
# (register MMIO, ring tracking, vblank, fence/writeback packets, swap
# counters) but renders nothing: draws/copies are counted and skipped and
# no provider or presenter is created. For per-title bring-up and for
# isolating guest CPU cost from GPU emulation cost.
#==============================================================================
set(REXGPU_STUB_SOURCES
    stub/plugin_main.cpp
    stub/graphics_system.cpp
    stub/command_processor.cpp

    # Shared, game-agnostic PM4 + Xenos base (same subset rexgpu-native
    # compiles for itself; the stub uses none of the rendering backends).
    flags.cpp
    register_file.cpp
    registers.cpp
    sampler_info.cpp
    graphics_system.cpp
    command_processor.cpp
    packet_disassembler.cpp
    # command_processor.cpp references g_shmem_force_log_until_ns
    shared_memory.cpp
)

add_library(rexgpu-stub SHARED ${REXGPU_STUB_SOURCES})
add_library(rex::gpu-stub ALIAS rexgpu-stub)

set_target_properties(rexgpu-stub PROPERTIES
    EXPORT_NAME gpu-stub
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(WIN32)
    set_target_properties(rexgpu-stub PROPERTIES PREFIX "")
endif()

target_include_directories(rexgpu-stub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REXGLUE_ROOT}/thirdparty/renderdoc
)

target_compile_options(rexgpu-stub PRIVATE -Wno-unused-variable)

target_link_libraries(rexgpu-stub PRIVATE
    rexruntime
    xxHash::xxhash
)

#==============================================================================
# rexshader-translator (rex::shader-translator): linkable Xenos shader
# translator for native device layers inside a port.
#
# Exactly the microcode -> SPIR-V translator and the Xenos register/ucode
# model, and nothing of the emulation around it: no EDRAM render-target cache,
# no write-watch shared memory, no primitive processor, no PM4 command
# processor. A port links it with
#     target_link_libraries(<port> PRIVATE rex::shader-translator)
# after rexglue_setup_target(). See docs/native/SHADER_TRANSLATOR_TARGET.md.
#
# Compiled separately from the plugins on purpose: the plugins build with
# hidden visibility and their own flags.cpp, and a static library shared with
# them would either export their internals or collide on cvar registration.
#==============================================================================
if(REXGLUE_USE_VULKAN)
    if(REXGLUE_GPU_BUILTIN)
        # The builtin Xenos backend is already a static library linked into
        # rexruntime; a second copy of these objects would duplicate symbols.
        # Reuse it (this drags the rest of the backend along, which the
        # builtin title carries anyway).
        add_library(rexshader-translator INTERFACE)
        target_link_libraries(rexshader-translator INTERFACE
            rexgpu-xenos Vulkan::Headers glslang::SPIRV renderdoc)
    else()
        set(REXSHADER_TRANSLATOR_SOURCES
            # Register / microcode model.
            register_file.cpp
            registers.cpp
            format/ucode.cpp
            # Shader analysis, ucode disassembly and the translator base.
            pipeline/shader/shader.cpp
            pipeline/shader/translator.cpp
            pipeline/shader/translator_disasm.cpp
            # Standalone cvar storage and the two draw_util constant tables
            # (see the files for why they are not taken from flags.cpp /
            # util/draw.cpp).
            pipeline/shader/standalone_flags.cpp
            pipeline/shader/standalone_draw_util.cpp
            # Microcode -> SPIR-V.
            ${REXGPU_XENOS_SPIRV_SOURCES}
        )
        add_library(rexshader-translator STATIC ${REXSHADER_TRANSLATOR_SOURCES})
        set_target_properties(rexshader-translator PROPERTIES
            POSITION_INDEPENDENT_CODE ON
        )
        target_compile_definitions(rexshader-translator PRIVATE
            REX_SHADER_TRANSLATOR_STANDALONE=1)
        target_compile_options(rexshader-translator PRIVATE -Wno-unused-variable)
        rex_target_pch(rexshader-translator
            <rex/math.h>
            <rex/memory.h>
            <rex/graphics/xenos.h>
            <rex/graphics/registers.h>
            <rex/graphics/format/ucode.h>
            <rex/graphics/pipeline/shader/shader.h>
        )
        # PUBLIC: the headers a consumer includes (spirv_translator.h ->
        # ui/vulkan/device.h -> Vulkan + renderdoc; spirv_builder.h ->
        # SPIRV/SpvBuilder.h) and the runtime whose cvar/log/assert/filesystem
        # services the sources call. The glslang internals are link-only.
        target_link_libraries(rexshader-translator
            PUBLIC
                rexruntime
                Vulkan::Headers
                glslang::SPIRV
                renderdoc
            PRIVATE
                glslang
                MachineIndependent
                GenericCodeGen
                OSDependent
                OGLCompiler
        )
    endif()
    add_library(rex::shader-translator ALIAS rexshader-translator)
    set_target_properties(rexshader-translator PROPERTIES EXPORT_NAME shader-translator)

    # Host test: translates every dumped ucode file in a directory. Not part of
    # ALL; build with --target shader_translator_test.
    if(EXISTS ${REXGLUE_ROOT}/tests/shader_translator/CMakeLists.txt AND NOT REXGLUE_GPU_BUILTIN)
        add_subdirectory(${REXGLUE_ROOT}/tests/shader_translator
            ${CMAKE_BINARY_DIR}/tests/shader_translator EXCLUDE_FROM_ALL)
    endif()
endif()
