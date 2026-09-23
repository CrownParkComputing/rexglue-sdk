# ReXGlue SDK change log — 2026-09-23 working tree

This document describes every change in the current working tree of
`_library/rexglue-vmx` relative to committed HEAD (`79bd7cd`), plus the SDK
commits the current port builds already carry. 73 files changed, ~2,400
insertions, ~320 deletions uncommitted.

Sections are ordered by user impact: launcher-visible behavior first, then
performance, then platform/kernel plumbing, then tooling and build system.

---

## 1. Launcher-visible behavior

### FPS cap selections are now real (`vsync_fps_cap`)

- `src/graphics/command_processor.cpp`

The launcher has always passed `--vsync_fps_cap=30/60/120/240` per Play, but
the cvar was only ever *queried* (`graphics_system.cpp` paces the guest vblank
worker from it) and *set* (`raylib_display.cpp`) — nothing registered it, so
every capped selection silently returned 0 and the guest pacer stayed at the
title's 60 Hz video mode. Symptoms: Split/Second stuck at 30 FPS (it presents
every second vblank) no matter what was selected, and every other title
topping out at 60. The cvar is now registered (default 0 = use the video
mode), and the registration-time pending-value path applies the command-line
value. Verified on Daytona USA: free-run 626 FPS → 30.1 FPS at cap 30.

Note for validation: with the cap live, titles that pace their simulation on
vblank can run fast or slow at off-native rates. This is a per-title property
and is recorded in each port's launcher review.

### Side rails and on-screen FPS for every title

- `src/ui/overlay/side_panels.cpp`, `include/rex/ui/overlay/side_panels.h`,
- `src/ui/style.h`, `include/rex/ui/style.h`, `src/ui/keybinds.cpp`,
- `include/rex/ui/keybinds.h`

The RetroRecomp side rails (previously MCLA-only) and an on-screen FPS counter
now apply to every title. Rails draw the collection brand and per-title panel
title (`side_panel_title` in the port config); F8 toggles them. `show_fps`
cvar controls the counter.

### Achievements and leaderboards overlays

- `src/ui/overlay/achievements_overlay.cpp`,
- `include/rex/ui/overlay/achievements_overlay.h`,
- `src/kernel/xam/xam_ui.cpp`, `src/kernel/xam/apps/xgi_app.cpp`,
- `src/system/kernel_state.cpp`, `src/system/xam/user_profile.cpp`

The achievements overlay was reworked (icons, unlock state, gamerscore, per-
profile tracking). A title's own Achievements menu item now opens the overlay
(`achievements_ui` cvar, default on). Leaderboards open a local high-score
page of the same overlay (`high_scores_ui`): the real screens read through
XUserReadStats against a live session and cannot be fed local data, so the
menu item opens the local page instead of pretending to sign in.

### Headless mode answers system prompts

- `src/kernel/xam/xam_ui.cpp`

`headless = true` in a port config answers the guest's system prompts with
their default instead of drawing the runtime's ImGui dialog over the game.
There is one local profile and one storage device, so the prompts have no real
choice. This is what lets unattended/headless runs get past save dialogs.

### Offline-by-default network behavior

- `src/kernel/xam/xam_net.cpp`

New `xbox_live` cvar, default off: the socket stack starts but reports no
link — the same state as a console with the cable unplugged — so online menus
disable themselves and offline play is untouched.

---

## 2. GPU/Vulkan CPU-side performance (the MCLA campaign)

Measured on Midnight Club: LA city scenes (the CPU-bound baseline). The goal
is frame-time headroom for high refresh rates; the GPU was never the
bottleneck (GPU pass ~1.1 ms vs ~12–22 ms frames).

### Pipeline state-input cache

- `include/rex/graphics/vulkan/pipeline_cache.h`,
- `src/graphics/vulkan/pipeline_cache.cpp`

`ConfigurePipeline` rebuilt the full `PipelineDescription` from guest
registers and re-hashed it into the pipelines hashtable on every draw. Now the
inputs `GetCurrentStateDescription` consumes are matched against a 64-entry
MRU of recent draws; a hit skips the register walk and the hashtable lookup.
The async placeholder-to-real hot-swap is still observed via atomic reload,
and in-flight async pipelines fall through to the full path.

Two properties were found by measurement, not guessed:

- A single-entry cache hits only strictly consecutive repeats (~15% on MCLA,
  which alternates materials constantly).
- Hashing whole registers misses almost always, because
  `VGT_DRAW_INITIATOR::num_indices` and the blend controls of absent render
  targets carry per-draw garbage. The key is masked to the fields the
  description actually reads. With masking the MCLA city route hits ~95%.

Invariant: `PipelineStateInput` must stay in sync with
`GetCurrentStateDescription`; a field it starts reading must be added to the
key. Diagnostic switch: `--vulkan_pipeline_state_hash_cache=false`. Frame-stat
CSVs append `pipeline_state_hits`, `pipeline_state_lookups`.

### Texture fetch-write memoization

- `include/rex/graphics/pipeline/texture/cache.h`,
- `src/graphics/pipeline/texture/cache.cpp`

Fetch constant writes marked the affected slots out of sync unconditionally,
so a title re-poking a whole fetch block per material made `RequestTextures`
re-parse constants, reconvert swizzles and redo the `TextureKey` lookup for
values it already holds. The write hook now compares the raw 24-byte fetch
constant against the last walked value and only drops the slot on a real
change. 13–30% of MCLA's ~6–13k fetch writes per frame are identical
rewrites and now cost nothing.

Diagnostic switch: `--texture_fetch_write_memoization=false`. CSVs append
`tex_fetch_writes`, `tex_fetch_unchanged`.

Note: `TextureFetchConstantsWritten` moved out-of-line from the header into
`cache.cpp`, which compiles into each GPU plugin — after touching this header,
rebuild the plugin fully; a partial build produced a plugin that fails to
dlopen (undefined symbol).

### Frame-local material descriptor cache

- `src/graphics/vulkan/command_processor.cpp`,
- `include/rex/graphics/vulkan/command_processor.h`

Texture descriptor sets are keyed by layout plus image views/samplers/layouts;
materials recurring non-adjacently in one frame reuse the prior set instead of
reallocating and rewriting descriptors. Measured: ~850 fewer descriptor
writes per frame (~2,046 reuse hits, writes 1,253 → 404). Cleared at frame
open; sets stay in the in-flight lifetime queue so nothing is rewritten while
the GPU may reference it. Diagnostic: `--vulkan_reuse_material_descriptor_sets=false`;
CSV column `texture_sets_material_reused`.

### Stable-binding fast path with per-slot epochs

- `include/rex/graphics/vulkan/texture_cache.h`,
- `src/graphics/vulkan/texture_cache.cpp`,
- `src/graphics/vulkan/command_processor.cpp`

When shaders, fetch constants of used slots, binding epochs, layouts and
resolved samplers match a recent state, `UpdateBindings` skips image-view
resolution. 16-entry MRU; last entry deep-compared directly; FNV hash only
computed when scanning the rest. Per-slot epochs bump on binding refresh and
texture destruction so a stale `VkImageView` can never be cached. Measured:
~1,800–1,900 hits/frame, `UpdateBindings` 3.2% → ~1.9% of CPU. Diagnostic:
`--vulkan_texture_binding_fast_path=false`; CSV column
`texture_sets_binding_fast_path`.

### Run-length float constant copies

- `src/graphics/vulkan/command_processor.cpp`

The float-constant upload loops copied one 16-byte constant per
`bit_scan_forward` iteration; constant maps are mostly dense runs, now copied
with one `memcpy` per run. (`bit_scan_forward` was ~1.8% of all CPU.)

### Barrier source tagging

- `src/graphics/vulkan/command_processor.cpp`,
- `include/rex/graphics/vulkan/command_processor.h`,
- `src/graphics/vulkan/texture_cache.cpp`

`SubmitBarriers` takes a source tag (texture / shared memory / render target /
other) and frame-stat CSVs count barriers per source (`b_tex`, `b_shmem`,
`b_rt`, `b_other`), so render-pass breaks are attributable.

### Shared-memory upload and residency batching

- `src/graphics/shared_memory.cpp`, `include/rex/graphics/shared_memory.h`,
- `src/graphics/vulkan/shared_memory.cpp`,
- `src/graphics/vulkan/command_processor.cpp`

Hot streaming-pool invalidations no longer force unrelated static vertex
ranges through the deferred upload path; the exact range is checked for
GPU residency first. Deferred ranges are sorted and merged before resident-
range checks. The hot-page cache records real draw demand separately from
speculative prefetch; unused prefetched pages expire instead of being recopied
indefinitely. `gpu_hot_page_frames` treats blocks dirtied N frames running as
permanently dirty (upload once per frame at frame open instead of trap +
re-arm per write under the kernel-wide lock). Worth 12 → 20 FPS on Daytona;
configs ship per-title values with the hazard documented.

### Per-frame CPU instrumentation

- `src/graphics/vulkan/command_processor.cpp`

`--gpu_frame_stats_path=<csv>` writes per-frame stage timings: draw CPU, fence
wait, texture upload, pipeline, bindings, vertex buffers, submit, ownership,
upload events/pages, render passes, break reasons, GPU pass times, and the
cache counters above. This is the measurement backbone for all of the above
and for the per-title launcher validation.

---

## 3. Kernel, system and input plumbing

- `src/input/input_system.cpp`, `include/rex/input/input_system.h` —
  guest input suppression (`SetGuestInputSuppressed`): the launcher/host can
  zero guest-visible input without unwiring the device.
- `src/system/xfile.cpp` — `ReadScatter` fixes for multi-segment reads.
- `src/kernel/xboxkrnl/xboxkrnl_io.cpp` — `NtCreateFile` handling for paths
  seen in newer ports.
- `src/system/xmemory.cpp` — access-violation callback handling tightened.
- `src/system/function_dispatcher.cpp` — invalid-function trap reporting
  (names the missing import instead of a bare abort).
- `src/system/runtime.cpp` — shutdown ordering fix.
- `src/system/xam/user_profile.cpp` — profile setting load defaults.
- `src/kernel/xam/xam_user.cpp` — `XamUserAreUsersFriends` answered locally.

## 4. Tools and build system

- `tools/new_port.sh` — stamps out a complete port: config profile, content
  packaging, headless testing, per-frame measurement, and a run script that
  cannot pick up stale SDK libraries.
- `tools/headless_play.sh.in` / `tools/measure.sh.in` — headless gameplay on
  gamescope's private display with scripted held keys, PipeWire frame capture
  (X11 grabs come back black; the stream node is the only output), per-stage
  frame-cost medians, and automatic sync of freshly built SDK libraries so a
  measurement never silently runs stale code.
- `tools/port_check.py`, `tools/port_doctor.sh`, `tools/fsb_tool.py` — port
  health checks and FSB audio tooling.
- `cmake/rexglueConfig.cmake.in`, `cmake/rexglue_helpers.cmake`,
  `cmake/rexglue_install.cmake`, `src/ui/CMakeLists.txt`,
  `src/graphics/CMakeLists.txt`, `thirdparty/CMakeLists.txt` — install/package
  layout for the SDK as a CMake package consumed by ports (`rexglue_DIR`),
  per-config plugin postfixes, GPU plugin staging next to the executable.

## 5. Committed work the current builds carry (for completeness)

- `79bd7cd` dynamic UBOs for guest constants (`vulkan_dynamic_constant_buffers`).
- `5bdc173` bulk-write register ranges; per-write debug-hook checks hoisted.
- `f514f2d` `db16cyc` delay hints lowered to `sched_yield`.
- `d05c5b3` Android thread-naming fix; xenos GPU default.
- `077c191` RetroRecomp side rails + on-screen FPS (initial version).

---

## Known open items in this tree

- Split/Second menu/title text distortion (blue smeared block where the title
  text should be) — reproduced with all new caches disabled, so it predates
  this campaign. Looks like a bytes-per-texel/pitch mismatch on a dynamically
  rendered text texture. Not yet fixed.
- Split/Second 30 FPS is a guest-side design lock (presents every second
  vblank); correct-speed 60 FPS needs timestep decoupling in the title, not
  renderer work. See the port's `docs/d3d/PACING.md`.
- `UpdateBindings` (~1.7% CPU), `TextureKey` lookups + `GetView` (~1.8%),
  `RenderTargetCache::Update` (~0.9%), `UpdateSystemConstantValues` (~0.9%)
  remain the top shared-runtime CPU costs after this campaign.
