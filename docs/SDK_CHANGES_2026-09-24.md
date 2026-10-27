# SDK changes, 2026-09-24: PGR4 crowd-palette bug hunt (Vulkan backend)

Context: PGR4's crowd skinning palette is byte-correct when the guest ends the frame and when
the command processor stages it, yet one crowd instance renders with a vertex flung to infinity
in ~15% of race frames, once every ~21 presented frames (`PORT/docs/d3d/M1.md`, `M1b.md`).
This note records what was added to the SDK to find the stage where the data goes wrong, and
what each run showed. All additions are diagnostics behind cvars that default off; the only
behaviour change is listed under "Behaviour changes".

## Diagnostics added (all default off)

| cvar | where | what it checks |
|---|---|---|
| `gpu_check_instanced_vs=<ucode hash>` | `src/graphics/vulkan/command_processor.cpp`, `IssueDraw` | For every draw with that vertex shader, at record time: `c52..c55` and `c255` sane (finite, `c55.x` integral, `c255 = (4, 0.5)`), the host draw path is the plain vertex-shader path over the guest DMA index buffer with the guest's counts, no vertex clamp, the 16-bit guest index buffer holds no index `>= c55.y` (which would make the shader's `a0` non-zero and read the next constant block), and, walking index -> mesh bone byte -> palette bone `c55.x + bone`, no referenced bone has a finite half `> 4000`. Logs `[ICHECK]` per violation and a per-swap summary. |
| same cvar | `UpdateBindings` / `BeginSubmission` | The VS float-constant block and the fetch-constant block written to the uniform pool for those draws are XXH3-hashed when written and re-hashed when their frame is observed complete, before `uniform_buffer_pool_->Reclaim`. Logs `[UNIFORM]`. |
| `gpu_check_staging_range=lo-hi` or `auto` | `src/graphics/vulkan/shared_memory.cpp`, `UploadRanges` / `CompletedSubmissionUpdated` | Every staging chunk intersecting the range is hashed after the memcpy and re-hashed when its submission's fence is observed complete, before `upload_buffer_pool_->Reclaim`. `auto` follows the largest vertex buffer of the checked shader's draws. Logs `[STAGING]`. |
| `gpu_check_gpu_readback_every=N` | `IssueDraw` tail | Every Nth checked draw per swap, copies the draw's largest vertex buffer out of the GPU shared-memory buffer, waits for the queue, and compares it byte-for-byte with guest memory. Logs `[GPUREAD]`. Slow (one queue drain per readback). |
| `gpu_check_ib_replay` | `src/graphics/command_processor.cpp`, `INDIRECT_BUFFER` | Hashes each indirect buffer before and after it is parsed (guest rewriting its push buffer under the CP) and compares a re-submitted `(ptr,len)` within one swap against its first pass, listing the differing dwords. Logs `[IBCHECK]`. |
| (always on, one line per 2 s) | `[KICKS]` statistics line | now also counts `LOAD_ALU_CONSTANT` packets. |
| (always on, capped at 20 reports) | `BeginSubmission` | `[SUBMISSION]`: reports deferred commands recorded while no submission was open (they would be discarded). |

`[NANKILL]` translation log (`spirv_translator.cpp`) now prints on every change of the kill state
with the shader hash instead of once per process.

Support code: `DeferredCommandBuffer::empty()`, `size_bytes()`, `DescribeCommands()`
(`include/rex/graphics/vulkan/deferred_command_buffer.h`, `src/graphics/vulkan/deferred_command_buffer.cpp`).

## Behaviour changes

* `VulkanCommandProcessor::EndSubmission` now calls `deferred_command_buffer_.Reset()` right
  after `Execute()`. Previously the executed stream stayed in the deferred buffer until the next
  `BeginSubmission` reset it; nothing read it in between, so this is inert for rendering, but it
  is what makes the `[SUBMISSION]` check meaningful.

## Runs and findings

All runs: `PORT/tools/headless_play.sh 230 "30:Return 45:Return 66:space ... 150:space"`, dumps
`/home/jon/pgr4dump/sw/<run>/f__NNNN.ppm` (frames 4300+, every 3rd), `triangle_detect.py`
default settings, logs `PORT/out/build/linux/logs/projectgotham4_1[3-6]x.log`. Harness and
analysis scripts in `/home/jon/pgr4dump/sw/` (`sweep230.sh`, `ctl.sh`, `correlate.py`,
`cmp_cp_guest.py`, `replay.py`).

| run | what | flagged | result of the check |
|---|---|---|---|
| inst1 (log 141) | `gpu_check_instanced_vs` + `gpu_check_ib_replay` + staging range at the M1 addresses | 60/380 (15.8%) | 2.7 M crowd draws: 0 constant/index violations. IB replays differ from the first pass only in 247 evenly spaced `FFFFFFFF -> 8000000F` words (per-tile predication masks), never rewritten during parse. Staging range did not match this run's pool address (it moves between runs). |
| inst2 (log 144) | staging range `auto` | 51/372 (13.7%) | 1501 staging chunks of the palette re-hashed at fence completion: 0 mismatches. |
| inst3 (log 147) | + uniform pool check | (dumps lost to a harness race) | 2.9 M VS-float/fetch constant blocks re-hashed at frame completion: 0 mismatches; no deferred commands ever recorded outside a submission. |
| inst4 (log 154) | + `gpu_check_gpu_readback_every=400` | 17/59 (slow run) | 1200 GPU readbacks of the palette after crowd draws: byte-identical to guest memory. |
| inst5 (log 156) | + per-draw CP dump vs the port's `pgr4_crowd_probe` guest dump, frames 4440-4558 | 23/83 | Swap = probe frame (offset 0) for all 118 frames; every crowd draw's `c52..c55` at CP record time equals the guest's constant shadow at the draw call, in flagged frames too. `LOAD_ALU_CONSTANT` is used ~3700/s by PGR4 but never for c52..c55. |

So between the guest's draw call and the GPU there is no stage where the palette, the index
buffers, the per-draw constants or their host copies differ from what the guest wrote.

| inst6 (log 158) | `gpu_check_instanced_dump`: per crowd draw the CP's `c4..c7`, `c52..c55`, index buffer, mesh and per-swap palette bytes, 383 swaps | 20/80 | Offline software replay of VS `72EE8062BF5E764A` from exactly these inputs (`/home/jon/pgr4dump/sw/replay.py`): every swap, flagged or not, contains ~9000 triangles that mix an on-screen vertex with a vertex whose position is NaN (bones 10, 13, 16-21 carry NaN translations by format), and the few "in front but far off-screen" triangles are the same draws in every swap. Nothing in the shader-visible inputs distinguishes a flagged swap. |

| nankill1-3 (logs 159, 160, 162) | `gpu_vertex_nan_kill=true` (its default is false; the earlier session's note that it was on is wrong - every M1/M1b run had `nan_kill=0`) | 63/373, 68/367, 63/363 (17-18.5%) | nankill3's per-shader log confirms `72EE8062BF5E764A` was translated with `nan_kill=1`. No change. |

### Cvar sweep (no code change, one 230 s run each, plain guest path, same script)

| run | cvar | flagged |
|---|---|---|
| bulk0 (log 163) | `gpu_bulk_register_writes=false` | 66/369 (17.9%) |
| sparse0 (164) | `vulkan_sparse_shared_memory=false` | 68/370 (18.4%) |
| nosub (165) | `vulkan_submit_on_primary_buffer_end=false` | 69/373 (18.5%) |
| nodynr (166) | `vulkan_dynamic_rendering=false` | 64/366 (17.5%) |
| cmps0 (167) | `clear_memory_page_state=false` | 73/352 (20.7%) |
| rthost (168) | `render_target_path_vulkan=host` | 57/391 (14.6%) |
| nodesc (172) | `vulkan_reuse_texture_descriptors=false`, `vulkan_texture_binding_fast_path=false`, `vulkan_reuse_material_descriptor_sets=false` | 63/377 (16.7%) |
| valid (171) | `vulkan_validation_enabled=true` | the title never reached the race in 230 s (see log) |

Baseline in this session: 13.7-18.5% (inst1, inst2, nankill1-3). Every configuration is inside
that band; nothing in the Vulkan backend's switchable behaviour changes the rate, and the
previous session's `gpu_invalidation_widening_pages=0`, `gpu_max_frames_ahead`,
`gpu_max_kicks_ahead`, `gpu_min_kick_lag_ms` and `pgr4_d3d_own_dynamic` runs were in the same
band.

Replay note: the crowd vertex shader is index-instanced (`instance = trunc((index + c255.y) /
c55.y)`, `a0 = 4 * instance`, per-instance constants `c[52+a0..55+a0]`, palette bone
`c[55+a0].x + mesh bone byte`); every race draw is single-instance (`index < c55.y` verified for
2.7 M draws), so `a0` is always 0.

Two further facts from the record-time skinning-input check (`palette(...)` violations):

* Bone 15 of every crowd pose carries a ~4500 translation (all frames, normal).
* In bursts of 5-15 consecutive swaps (e.g. 4201-4207, 4226-4232, 4310-4314, 4438-4453) a
  referenced body bone (0, 1, 4, 9 ...) has a finite translation growing by ~6850 per frame
  from normal to ~40000 and back: the crowd animation interpolating a bone towards its far
  "park" position. This is real guest data. These bursts coincide with some flagged frames
  (4446, 4449) but not with the regular 21-frame cadence (4467, 4488, 4509, 4551 ...), whose
  End-time palettes (`m1b1` dumps) contain no finite half above 6600 and no infinities.

## Where this leaves the bug

Every stage between the guest's draw call and the Vulkan queue has now been checked in the
failing runs and found byte-exact: per-draw constants (CP register file vs guest shadow, and the
uniform-pool copy at frame completion), the fetch constants, the index buffer and mesh, the
palette (guest memory vs staging chunk at fence completion vs a GPU readback after the draw),
the indirect buffers (never rewritten during parse; replays differ only in predication masks),
the deferred command stream (never recorded outside a submission), descriptor-set and pool
recycling (frame-fenced). An offline replay of the vertex shader from the CP's inputs produces
the same geometry for flagged and clean swaps. No switchable backend behaviour and neither the
NaN kill nor the render-target path changes the 1-in-21-frames rate.

| inst7 (log 174) | dump record v2: per crowd draw also viewport, scissor, XXH3 of `SystemConstants`, pipeline handle (`/home/jon/pgr4dump/sw/hostdiff.py`) | 16/72 | For flagged swaps 4449 and 4473, no crowd draw has host state different from the same draw index in both neighbouring clean swaps; viewport `0,0 1280x720`, scissor `1280x480`, one pipeline for the whole race. |

What is not covered, and is where the next pass should go:

1. **The GPU's execution of otherwise identical command streams.** With per-draw host state
   also identical, the remaining explanation is host-GPU behaviour on this geometry (thousands of triangles per
   frame span the near plane with `w` near 0, and thousands mix NaN vertices even with the kill
   compiled in; both are undefined-ish for the clipper). The test is a RenderDoc capture of a
   flagged and a clean frame (`rex::ui::vulkan::RenderDocAPI` is wired in `vulkan_instance.cpp`)
   or the same frame replayed twice from the `gpu_check_instanced_dump` data on a standalone
   Vulkan harness. The validation-layer runs (`valid`, `validsync`) did not reach the race in
   230 s; a longer run (400 s+) with `--vulkan_validation_enabled` is the cheap version of this.
2. **Tile replay.** Each crowd draw is issued twice per frame (predicated tiling, tile masks
   `8000000F` in the replayed IBs); the two passes carry identical state, but `gpu_skip_tile_replay`
   (one pass per IB) has not been A/B'd against the triangle rate in this session.

All diagnostics stay in the tree behind their cvars (default off, zero cost when off) so the
next pass can start from the CP dump and the replay script.
