# `rex::shader-translator`: the Xenos shader translator as a linkable target

Workstream A5 of the native-renderer programme. A native D3D9 layer inside a
port (`PORT/src/d3d9native/`) has to turn the precompiled Xenos microcode the
title ships into SPIR-V, and it has to reason about the Xenos register model
(fetch constants, `SQ_PROGRAM_CNTL`, `PA_CL_CLIP_CNTL`, ...). Until now both
lived only inside the GPU plugins (`rexgpu-xenos`, `rexgpu-native`), next to
the EDRAM render-target cache, the write-watch shared memory, the primitive
processor and the PM4 command processor. `rex::shader-translator` is exactly
the translator and the register/ucode model, and none of that emulation.

## What is in the target

CMake target `rexshader-translator` (alias `rex::shader-translator`, exported
as `rex::shader-translator`), defined at the end of
`src/graphics/CMakeLists.txt`, built when `REXGLUE_USE_VULKAN` is on.

| Source | Role |
|---|---|
| `register_file.cpp`, `registers.cpp` | `RegisterFile`, `reg::*` register structs, `register_table.inc` |
| `format/ucode.cpp` | microcode instruction decoding (`ucode::*`) |
| `pipeline/shader/shader.cpp` | `Shader`: ucode storage, `AnalyzeUcode()`, bindings, `DumpUcode()` |
| `pipeline/shader/translator.cpp`, `translator_disasm.cpp` | `ShaderTranslator` base: CF/ALU/fetch walk, disassembly |
| `pipeline/shader/spirv_builder.cpp`, `spirv.cpp`, `spirv_translator*.cpp` | `SpirvShader`, `SpirvShaderTranslator` (microcode -> SPIR-V) |
| `pipeline/shader/standalone_flags.cpp` | cvar storage for the standalone copy (see "cvars") |
| `pipeline/shader/standalone_draw_util.cpp` | the two D3D10 sample-position tables `spirv_translator_rb.cpp` reads |

Not in the target, by design: `pipeline/render_target/cache.cpp` (EDRAM),
`shared_memory.cpp` (write-watch buffer), `primitive_processor.cpp`,
`command_processor.cpp` (PM4), `pipeline/texture/cache.cpp`,
`pipeline/shader/interpreter.cpp`, `util/draw.cpp`, `flags.cpp`, and anything
under `vulkan/` or `native/`.

Link interface: `PUBLIC rex::runtime Vulkan::Headers glslang::SPIRV renderdoc`
(the public headers need them: `spirv_translator.h` includes
`rex/ui/vulkan/device.h`, `spirv_builder.h` includes `SPIRV/SpvBuilder.h`), and
`PRIVATE` the glslang internals (`glslang MachineIndependent GenericCodeGen
OSDependent OGLCompiler`), which the export carries as `$<LINK_ONLY:...>`.

The plugins do **not** consume this target; they keep compiling their own copy
of the same sources. Reason: they build with hidden visibility and their own
`flags.cpp`, and sharing objects would either export their internals or
collide on cvar registration (below). Duplicate compilation is accepted for
now.

`REXGLUE_GPU_BUILTIN=ON` (static title builds): `rexshader-translator` becomes
an INTERFACE target that links the builtin `rexgpu-xenos` static library
instead, because a second copy of these objects would duplicate symbols. That
configuration therefore does drag the rest of the backend along.

## Linking it from a port

New ports: the generated `rexglue.cmake` carries

```cmake
rexglue_link_shader_translator(<port-target>)   # after rexglue_setup_target()
```

Existing ports (or by hand): after `rexglue_setup_target(<port>)` add

```cmake
target_link_libraries(<port> PRIVATE rex::shader-translator)
```

Both the installed SDK (`find_package(rexglue)`) and the source-tree mode
(`REXSDK_DIR`) provide the `rex::shader-translator` name. Requires an SDK built
from this change or later; the exported set previously had no such target.

Then in C++:

```cpp
#include <rex/graphics/pipeline/shader/spirv.h>                       // SpirvShader
#include <rex/graphics/pipeline/shader/spirv_translator.h>            // SpirvShaderTranslator
#include <rex/graphics/pipeline/shader/spirv_translator_features.h>   // Features without VulkanDevice
```

## Constructing the translator without a `ui::vulkan::VulkanDevice`

`SpirvShaderTranslator` takes a plain `SpirvShaderTranslator::Features` struct;
only the convenience constructor `Features(const ui::vulkan::VulkanDevice*)`
touches the presenter's device class (header-only, no link dependency). A port
with its own `VkPhysicalDevice` uses the new header
`rex/graphics/pipeline/shader/spirv_translator_features.h`:

```cpp
using namespace rex::graphics;
SpirvTranslatorDeviceInfo info = QuerySpirvTranslatorDeviceInfo(
    physical_device, vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceFeatures,
    vkGetPhysicalDeviceProperties2, vkGetPhysicalDeviceFeatures2,
    api_version /* physical device apiVersion clamped to the instance version */,
    /*ext_khr_spirv_1_4=*/enabled, /*ext_khr_shader_float_controls=*/enabled,
    /*ext_ext_fragment_shader_interlock=*/enabled,
    /*ext_ext_shader_demote_to_helper_invocation=*/enabled);
// info describes what the device SUPPORTS; clear anything you did not enable on
// the VkDevice (the translator emits code that relies on these).
SpirvShaderTranslator translator(MakeSpirvTranslatorFeatures(info),
                                 /*native_2x_msaa_with_attachments=*/true,
                                 /*native_2x_msaa_no_attachments=*/false,
                                 /*edram_fragment_shader_interlock=*/false);
```

`Features(bool all)` also exists: `Features(true)` is "everything, SPIR-V 1.5",
`Features(false)` is a bare Vulkan 1.0 device.

Translating one shader (what `rexgpu-native` does in `IssueDraw`):

```cpp
SpirvShader shader(xenos::ShaderType::kVertex, hash, guest_dwords, dword_count,
                   std::endian::big);          // guest memory is big-endian
rex::string::StringBuffer disasm;
shader.AnalyzeUcode(disasm);                   // once per shader

SpirvShaderTranslator::Modification mod(translator.GetDefaultVertexShaderModification(
    shader.GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg)));
mod.vertex.interpolator_mask = interpolator_mask;   // from the pixel shader's needs
mod.vertex.user_clip_plane_count = ...;             // from PA_CL_CLIP_CNTL
Shader::Translation* t = shader.GetOrCreateTranslation(mod.value);
if (!t->is_translated()) translator.TranslateAnalyzedShader(*t);
if (t->is_valid()) { const std::vector<uint8_t>& spirv = t->translated_binary(); ... }
```

The shader hash the plugins use is `XXH3_64bits` over the guest (big-endian)
bytes; `--dump_shaders` files are named by it.

## ABI the native device must honour

The SPIR-V the translator emits is not free-standing: it expects the same
binding model the Vulkan backend provides. The authoritative constants are in
`include/rex/graphics/pipeline/shader/spirv_translator.h` (`kDescriptorSet*`,
`SystemConstants`, `kSysFlag_*`) and `vulkan/command_processor.cpp` shows a
complete implementation. In short:

**Descriptor sets** (`SpirvShaderTranslator::kDescriptorSet*`, in binding order):

| Set | Contents |
|---|---|
| `kDescriptorSetSharedMemoryAndEdram` (0) | the whole 512 MB guest physical memory as storage buffer(s), plus the EDRAM buffer when interlock is used |
| `kDescriptorSetConstants` (1) | uniform buffers: system constants, float constants (VS then PS), bool/loop constants, fetch constants |
| `kDescriptorSetTexturesVertex` / `kDescriptorSetTexturesPixel` | sampled images and samplers per fetch constant used by the shader |
| `kDescriptorSetMutableLayoutsStart...` | see the header; layouts after the fixed sets are per-shader |

**Shared memory (in-shader vertex fetch).** Xenos shaders do their own vertex
fetch: `vfetch` reads guest physical addresses from fetch constants. The
translator turns that into loads from a storage buffer that must alias the
entire guest physical address space (address 0 of the buffer == guest physical
0). With `max_storage_buffer_range < 512 MB` the buffer is split into
`1 << GetSharedMemoryStorageBufferCountLog2(range)` consecutive bindings of
equal size. The native device does not need the write-watch machinery of
`shared_memory.cpp`, but it does need a buffer (or a set of buffers) with
exactly this layout, kept coherent with guest memory before each draw for the
ranges the draw reads. Index buffers, memexport and texture uploads go through
the same address space.

**Constant buffers laid out like the register file.**
- Float constants: 256 `float4` for VS (`SHADER_CONSTANT_000_X`..`_255_W`) and
  256 for PS (`SHADER_CONSTANT_256_X`..`_511_W`), each a contiguous copy of the
  register range - the shader indexes them directly (`c[n]`), including
  relative addressing by `a0`/`aL`.
- Bool constants: `SHADER_CONSTANT_BOOL_000_031`..`_224_255` (8 dwords); loop
  constants: `SHADER_CONSTANT_LOOP_00`..`_31` (32 dwords); one buffer, bools
  then loops.
- Fetch constants: `SHADER_CONSTANT_FETCH_00_0`..`_31_5` (32 x 6 dwords),
  verbatim register contents; vertex fetch constants are read in the shader,
  texture fetch constants are what the device decodes to build images and
  samplers.
All of these are the raw dwords of the register file at draw time; the
register indices are in `rex/graphics/register_table.inc` and the layouts in
`xenos.h` (`xe_gpu_vertex_fetch_t`, `xe_gpu_texture_fetch_t`).

**System constants** (`SpirvShaderTranslator::SystemConstants`, one uniform
buffer, exact field list and order in the header): the flags word
(`kSysFlag_*`: vertex index endian/load, user clip planes, alpha test, colour
exponent bias, gamma, ...), vertex index endianness and base, NDC scale/offset
that fold the Xenos viewport into Vulkan's, point size, texture swizzles and
signedness per fetch constant, alpha test reference, colour output exponent
bias, and the EDRAM/resolution-scale fields (which a native device fills with
neutral values: scale 1, interlock off). The Vulkan backend fills it in
`VulkanCommandProcessor::UpdateSystemConstantValues`; a native device has to
produce the same values from its own state shadow.

**Pipeline state derived from shader analysis.** After `AnalyzeUcode()` the
`Shader` reports what it needs: `GetTextureBindingsAfterTranslation()` /
`GetSamplerBindingsAfterTranslation()` (which fetch constants to bind as images
and samplers, in binding order), `writes_color_target(n)`, `writes_depth()`,
`kills_pixels()`, `memexport_eM_written()`, `uses_register_dynamic_addressing()`.
The `Modification` bits (interpolator mask, clip planes, point parameters,
depth/stencil mode, param-gen) are part of the pipeline key; the same ucode
yields one `Translation` per modification value.

**Output conventions.** Vertex shaders write `gl_Position` already in Vulkan
clip space (NDC scale/offset from system constants), clip/cull distances as
requested by the modification, and interpolators as flat `location`s matching
`interpolator_mask`. Pixel shaders write up to four colour outputs in the
Xenos render-target order; with `edram_fragment_shader_interlock == false`
render targets are plain colour attachments and depth is the fixed-function
depth attachment, which is the mode a native device wants.

## cvars

The translator sources read three cvars (`dump_shaders`,
`gpu_vertex_nan_kill_debug`, `texture_exp_bias_ignore`). A port that links
this target still loads a GPU plugin that registers those very names, and the
cvar registry drops a second registration of a name. The standalone copy
therefore registers its knobs as `shader_translator_dump_shaders`,
`shader_translator_vertex_nan_kill_debug` and
`shader_translator_texture_exp_bias_ignore` (`standalone_flags.cpp`) and the
sources compile with `REX_SHADER_TRANSLATOR_STANDALONE=1`, which is the only
`#if` added to translator code (`spirv_translator_fetch.cpp`, around the
`texture_exp_bias_ignore` definition).

## Host test

`tests/shader_translator/shader_translator_test.cpp`, target
`shader_translator_test` (not in ALL; added from `src/graphics/CMakeLists.txt`):

```
cmake --build out/build/linux-amd64 --target shader_translator_test
out/linux-amd64/Release/shader_translator_test [dir] [--features=all|min|both] [--show=HASH] [--dump=<dir>] [-v]
```

It loads every `shader_<hash>.ucode.bin.{vert,frag}` (raw host-endian ucode
dwords as written by `Shader::DumpUcode`), checks the hash against XXH3 of the
big-endian bytes, checks `AnalyzeUcode()` reproduces the sibling
`shader_<hash>.ucode.{vert,frag}` disassembly, and translates with the default
modification (all interpolators, no clip planes, no depth/stencil modifiers).
Baseline on the PGR4 dump (`/home/jon/pgr4dump/shaders`, 244 shaders): 244/244
translate with both `Features(true)` and the bare-1.0 feature set;
`72EE8062BF5E764A` and `7AB878B149287B5E` included.

## Coupling that remains

- `spirv_translator.h` still includes `rex/ui/vulkan/device.h` for the
  `Features(const VulkanDevice*)` constructor. Compile-time only (Vulkan
  headers + renderdoc header), no link dependency on `rexui`; left as is
  because the header is under active diagnostic edits.
- `spirv_translator_rb.cpp` references `draw_util::kD3D10StandardSamplePositions*`
  from `util/draw.cpp`; duplicated in `standalone_draw_util.cpp` rather than
  linking `draw.cpp` (which pulls in the texture cache and plugin cvars).
- The library links `rex::runtime` for cvar/logging/assert/filesystem/string;
  a port already links it.
- `REXGLUE_GPU_BUILTIN` builds get the whole builtin backend instead (see above).
