# Experimental native graphics package

The package replaces audited CPU graphics-library leaf routines with native
C++ helpers while retaining the generated command stream and its ordering.
The current profiles cover raw float4 constant setters and grouped constant
PM4 emission. They do not bypass PM4, move game logic to a GPU, or provide a
complete native D3D implementation.

Installation and runtime use are optional. The SDK install options
`REXGLUE_INSTALL_NATIVE_GRAPHICS` and `REXGLUE_INSTALL_NATIVE_BACKEND` default to `OFF`; the executable runtime
switch `XN_NATIVE_GFX` is also disabled unless explicitly set. Linux x86-64
is the tested host; the helpers forward on other hosts because their guest
memory alias conventions require separate verification.

## Installing

Configure an SDK build with `-DREXGLUE_INSTALL_NATIVE_GRAPHICS=ON`. The source
headers, scripts, CMake module and this document belong to the `NativeGraphics`
install component:

```sh
cmake --install <sdk-build> --component NativeGraphics --prefix <sdk-prefix>
```

This component does not install or overwrite a runtime/GPU library. A normal
complete SDK install with the option enabled includes the same package.
`find_package(rexglue CONFIG REQUIRED)` includes its CMake module when present.
Source-tree consumers may include `cmake/rexglue_native_graphics.cmake` directly.

The separate `NativeBackend` component installs the audited standalone native
platform sources, bundled dependencies, Nuklear and its licensed UI font.
Enable `REXGLUE_INSTALL_NATIVE_BACKEND=ON` and install that component to build
future ports without an external Forza checkout. Source files are listed and
hashed in a package manifest; original generated title C++, assets, saves,
caches and binaries are excluded. Its only binary resource is the pinned
licensed font. It compiles new title binaries; it does not replace the SDK's
existing runtime/GPU libraries or change their defaults.

## Binding a title

The port supplies its own audited TOML manifest and locally generated module.
Manifests contain function addresses, a verified profile, offsets/thresholds
and a SHA256 of each complete generated function body. Game code, shaders,
executables and saves are never installed by this package.

```cmake
find_package(rexglue CONFIG REQUIRED)
# Configure the ordinary generated-code executable first.
rexglue_attach_native_graphics(my_title
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/native_graphics.toml"
  GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
```

Call the attachment function once per executable target. Its generated hooks
must be direct executable sources, because a static archive containing strong
overrides may not be selected when a generated weak symbol already satisfies
the linker. A changed or missing original body fails configuration; it must
be audited again before the manifest is updated.

Run normal fallback behavior first, then use `XN_NATIVE_GFX=constants` for the
experiment. `XN_NATIVE_GFX_STATS=1` enables optional call/native/fallback/byte
counters; leave statistics disabled during performance comparisons. Programs
that use `_Exit` should call
`xn::gfx::adapters::ReportNativeGraphicsHooks()` after graphics work stops if
they want the final counters. Each helper rejects unsupported calls before
changing any guest memory or CPU context, then the hook runs its original.

## Verification

Run the installed Python generator as a script with Python 3.11 or newer.
The verifier extracts the audited originals only into a caller-selected local
ignored directory and compares complete guest memory and CPU contexts. With
an installed package, its header directory is the platform root; the SDK's
normal include directory and SIMDE include root supply the CPU ABI headers:

```sh
python3 <sdk-prefix>/share/rexglue/native_graphics/verify_hooks.py \
  --generated <port>/generated/default --manifest <port>/native_graphics.toml \
  --output <port>/out/native-graphics/oracle \
  --platform-root <sdk-prefix>/include/rex/native_graphics \
  --include-root <sdk-prefix>/include
```

An SDK source checkout can supply `include/` and `thirdparty/simde/` as separate
`--include-root` arguments. Keep original bodies, captures, caches and saves
out of source control. A synthetic oracle proves helper equivalence for its
tested contract; game hook reach, rendering and FPS still need runtime A/B
tests with the same scene, resolution and cache state.

This package is an experiment backed up independently with the Forza native
platform sources. It is not an upstream release or a general promise of 60 FPS.

## Standalone native Vulkan backend

The separate `rexglue_setup_native_target` function attaches the shared native
runtime, Vulkan renderer, audio and Nuklear UI to an existing executable.
It resolves the installed `NativeBackend` sources automatically, or accepts
an explicit compatible `PLATFORM_ROOT`. It uses locally generated code and
does not copy a game's executable or payload into the SDK package.

```cmake
# Source-tree use may include cmake/rexglue_native_target.cmake directly.
find_package(rexglue CONFIG REQUIRED COMPONENTS NativeBackend)
add_executable(my_title native_main.cpp)
rexglue_setup_native_target(my_title
  GENERATED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/generated"
  TITLE my_title MODULES default)
```

The caller supplies a standalone native `main` and title-specific timing/media
hooks; an SDK `RexApp` main is not interchangeable. The wrapper adapts build
copies of the generated C++, creates object libraries with PCHs, audits native
imports and links only `xn_runtime`, `xn_renderer_vulkan`, audio and the shared
menu. Forza-specific compatibility hooks remain conditional in the platform's
adapter. It does not link the SDK runtime or GPU plugin targets; a deferred
CMake dependency check rejects mixing those with the standalone runtime.
The wrapper also places the standalone CPU/kernel headers first for audio
imports. SDK-exported SDL3-static adds a general SDK include directory, which
must not select the older SDK hook/CPU contract for those native imports.

`OUTPUT_DIRECTORY` and `IMPORTS_OUTPUT` can retain an existing port's build
paths. Build-copy directories must be separate from original generated inputs.
An explicit `PLATFORM_ROOT` overrides the bundled default and must include
its adjacent resource/dependency directories. Target properties `REXGLUE_NATIVE_PLATFORM_ROOT`,
`REXGLUE_NATIVE_GENERATED_DIRECTORY` and `REXGLUE_NATIVE_OBJECT_TARGETS` expose
the resulting integration. Linux x86-64, Clang and C++23 are currently
required. Merely installing or finding
the package changes no existing title's backend.

A complete asset-free consumer was built and run from the real installed
`NativeBackend` sources with no explicit platform root. All 115 platform
translation units came from the installed source bundle, and its CPU-only
test exited successfully. Dynamic dependency inspection found no SDK runtime
or GPU plugin libraries. This validates package build/link integration;
rendering correctness and speed still require the title's runtime tests.

The source bundle also includes shared renderer capture/replay tools. Configure
with `-DXN_BUILD_RENDER_TOOLS=ON` to build `xn_render_replay` and the asset-free
CPU probes. See [the replay workflow](../../docs/RENDERER_REPLAY.md) for exact
image verification, comparison flags and timing limits. Command-state reuse is
enabled within the native backend. Dirty texture-state preparation is also
enabled for renderers without a host texture provider, with
`XN_VK_TEXTURE_DIRTY_CACHE=0` available for comparison. The optional
`XN_VK_DRAW_BATCH_PROFILE=1` reports consecutive draw compatibility but does
not combine submissions. CPU register batching and index/constant
reuse remain separate opt-in experiments. Installing this source component
still leaves existing SDK runtime and GPU library binaries unchanged.

Packed float-constant uploads now use CPU staging and immutable upload reuse
by default; `XN_VK_FLOAT_CONSTANT_REUSE=0` restores the previous path. This is
separate from the older three-block constant experiment above. GPU-addressed
draw records and actual consecutive multi-draw are available with
`XN_VK_MULTI_DRAW=1`; `XN_VK_GPU_CONSTANTS=1` tests the shader ABI independently.
Both GPU modes remain opt-in because the verified draw-count reduction has not
demonstrated an end-to-end frame-time improvement. Device feature/interface
limits select a compatible fallback automatically. The replay guide records
the measured CPU improvements and experimental limits.


The backend also carries gated follow-up experiments. Fused pending-state/draw
callbacks require both `XN_NATIVE_GFX_BATCH=1` and
`XN_CP_DRAW_STATE_BATCH=1`; recorder wrappers retain ordered callback fallback.
`XN_VK_PREPARED_INDEX_CACHE=1` retains bounded, version-checked CPU index
preparation across batches, while copying into fresh fenced GPU storage;
`XN_VK_PREPARED_INDEX_MIN_COUNT=512` can exclude smaller draws.
`XN_VK_READONLY_BARRIER_ELISION=1` skips only repeated shader-read or transfer-source
layout barriers, retaining draw flushes. These switches remain off by default:
initial exact-image comparisons showed workload-dependent results rather than
an across-the-board improvement.

The native build-copy adapter recognizes exact generated `db16cyc` delay hints.
Its helper retains scheduler yields by default; `XN_GUEST_DELAY_HINT=1` selects
an experimental processor hint with a periodic scheduler handoff. Original
generated inputs remain untouched. This is not a notification primitive or a
claim of lower total CPU usage. `XN_GUEST_REFRESH_HZ=120` separately tests guest
video/vblank refresh from process startup; default guest refresh remains 60 Hz.
The presentation cap alone does not select this guest refresh, and titles cache
video mode during device creation. Validate simulation, input, audio and frame
pacing before making a high-refresh mode a title default.


`XN_VK_RESIDENT_INDEX_CACHE=1` tests immutable GPU-resident DMA indices with
fenced eviction and ordinary ring fallback. It requires coherent host-visible
DEVICE_LOCAL memory, allocates after the original rings, and defaults to a
32-MiB arena/minimum 256 indices (`XN_VK_RESIDENT_INDEX_MIB` and
`XN_VK_RESIDENT_INDEX_MIN_COUNT`). GPU-constant/multi-draw modes retain their
compatible existing path. `XN_VK_PREPARED_DRAW_STATE=1` reuses active shader
entries and the last full pipeline key. Both are off by default: high cache-hit
and lookup-avoidance counts have not established a consistent speed improvement.
`XN_VK_INSTANCE_BATCH_PROFILE=1` with residency measures strict same-count
instancing candidates; it does not issue instanced draws. The native tools now
include an allocator probe covering pin/fence/rebind/eviction pressure and zero
steady hit/retirement allocations. Frame and replay diagnostics expose
user/system CPU time and resident-cache counters.


`XN_VK_DIRTY_DRAW_PREPARATION=1` tests register-driven draw preparation in the
shared renderer. It caches shader variant selection, viewport/scissor transforms,
static system constants and pipeline selection. VGT index values and texture
signedness patch their own system fields. Float uploads use exact shader-used
constant dirty bits; bool/loop, fetch and system offsets are reused only within
the current fenced upload batch. The cache never retains target pointers or
texture descriptor sets. Snapshot and Shape A feeds invalidate preparation,
resource uploads/resolves retain their ordinary ordering, and shutdown discards
cached modules/pipelines. `native.draw_preparation_dirty` and the GPU ring-feed
regression exercise register delivery, dirty fields and buffer retirement.
Replay JSON and frame CSV expose `preparation_*_hits`,
`dirty_constant_uploads_reused` and `float_constant_packs_avoided`.

The generic renderer default remains off. A validated title can set
`VulkanRendererConfig::dirty_draw_preparation = true`; the environment override
still takes precedence. MCLA enables this after its live map improved from
27.18 to 32.66 FPS in the tested window. Forza keeps its previous default because
its captures did not establish a benefit. The MCLA integration is preserved in
`mcla_dirty_draw_default.patch`; apply it in the sibling MCLA project.

Virtual EDRAM is bounded per renderer: `VulkanRendererConfig::virtual_edram_mib`
accepts 10 (default), 20 or 40. `XN_VK_VIRTUAL_EDRAM=0`, `1`, or `40` overrides
that with 10/20/40 MiB. Physical mode retains 11-bit wrapping; extended modes
honor the existing register bit 11, keeping 12-bit bases while allowing larger
extents. Allocation, tile masks, clear plans and alias/resolve bounds agree on
the selected capacity. `RendererStats::edram_tile_capacity` and frame/replay
metrics report the actual choice.

The native build-copy adapter attaches MCLA's two audited tiling hook sites only
for TITLE `midnightclubla`, without modifying original generated files. Its
helper requires renderer readiness, mapped instruction signatures, a supported
1280x720-or-smaller footprint, and either the backbuffer or the current scene
owner's two target slots. Other targets retain their original tiling. Surface
admission checks wide base+size equality and the 12-bit base limit. MCLA's
application integration, preserved in `mcla_single_tile.patch`, selects 40 MiB
and enables the scene path by default; `XN_MCLA_SINGLE_TILE=0` disables it.
Use both `XN_MCLA_SINGLE_TILE=0 XN_VK_VIRTUAL_EDRAM=0` for the original path.
Titles without their own capacity opt-in and the generic SDK default to 10 MiB.

`XN_MCLA_TILE_TRACE=/absolute/path.csv` records per-call tile decisions and a
separate allocation trace; it is off normally. It is not a per-PM4-frame census.
The CPU/GPU probes cover physical wrapping, virtual boundaries, upper-bank
color/depth aliases, footprint rejection, and the native adapter's source
anchors. Select `XN_VK_VIRTUAL_EDRAM=40` when replaying captures that use the
expanded range. Replay JSON exposes the selected capacity, and exact-image
verification checks the resulting pixels. The trace does not automatically
select the renderer capacity.

Forza's audited native adapter now joins its main 1280x720, 2x-MSAA race
descriptor into one rectangle before the title creates its scene surfaces.
The same descriptor supplies rendering and resolve rectangles, and the title
places depth after the enlarged color allocation (1440 tiles each). The hook
checks mapped instructions, the original two-strip layout and grid metadata;
other scene descriptors retain their original path. Forza's application main
selects 20 MiB and enables this by default. The generic renderer still defaults
to 10 MiB; MCLA retains its independent 40-MiB setting and title hook.

`XN_FORZA4_SINGLE_TILE=0 XN_VK_VIRTUAL_EDRAM=0` restores Forza's original
configuration. `XN_FORZA4_SCENE_TRACE=/absolute/path.csv` optionally records
descriptor decisions during surface initialization; it is not a per-frame
draw census. SDK consumers selecting TITLE `forza4` link the guarded helper,
but their own main must call `Forza4NativeConfigureTiling` after renderer
initialization and before launching guest threads. Supply the capacity
selected with `SelectVirtualEdramMiB` / `VirtualEdramTileCapacity`, including
the environment override. This also works when a capture wrapper owns the
renderer. Unconfigured helpers retain the normal title path.

## Original-file setup

The shared Nuklear library includes Forza's optional first-run wizard and
`forza4_setup.h` backend. Its importer and video-preparation scripts ship under
the bundle's `tools/` directory. Callers supply that directory in
`ForzaSetupPaths::tools_root`; installing the backend does not open the wizard
or import any content automatically.

`RunForzaSetupWizard` collects original Disc 1/Disc 2 sources, a player name,
and separate local-file and save/content destinations. `RunForzaSetup` exposes
the same installer without a window. Completion writes a validated setup
receipt in the selected profile; `LoadForzaSetup` returns the game, video and
save roots for the caller's launch configuration. This validator accepts the
audited Forza executable only. Other titles can reuse the menu and import
workflow but need their own validation and content layout.

Forza disables the runtime Player and Disc 2 tabs with
`NativeOptionsConfig::player` and `disc2`; other consumers retain their existing
defaults. Headless setup tests use private profiles and destinations, including
checks for retained save files and rejection of unrelated occupied folders.
