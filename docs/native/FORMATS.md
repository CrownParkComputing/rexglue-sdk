# Native migration: frozen interchange formats

Programme B (estate scaffolding) and Programme A (PGR4 native D3D9) exchange
data through the files below. The formats marked **frozen** do not change
without a version bump; the placeholders are owned by the workstream named
and are filled in when that workstream lands. Addresses are always guest
virtual addresses written as eight upper-case hex digits with no `0x` and no
`sub_` prefix (`823DDA50`), matching `generated/*/codegen.partition.json`.

## Reach trace CSV (frozen, Stage 0)

Produced by the runtime when a port's generated code is compiled with
`-DREX_GUEST_REACH` and the process runs with `REX_TRACE_REACH=<path>`.

```
addr,count
823DDA50,4812
82695BE0,301
hand_written_name,1
```

* First line is the header `addr,count`, always present.
* One line per guest function that was entered at least once; functions never
  reached are absent (a reader treats absence as count 0; join against
  `symbols.json` for the full generated set).
* `addr`: 8 hex digits parsed from the `sub_XXXXXXXX` part of the function's
  symbol name. A hand-written function using `REX_FUNC_PROLOGUE` without an
  address in its name emits its symbol name instead (rare; readers may skip
  rows whose `addr` is not 8 hex digits).
* `count`: decimal, 64-bit, calls since process start. Counts are cumulative
  and monotonic; each rewrite of the file replaces the previous one.
* Order is unspecified (registration order, newest first). Sort before diffing.
* The file is written atomically (`<path>.tmp` then rename) so a reader never
  sees a partial file. It is rewritten every `REX_TRACE_REACH_INTERVAL_MS`
  milliseconds (default 5000; `0` disables the periodic writer), at normal
  exit (`atexit`) and on `SIGTERM`/`SIGINT` (the previous handler or default
  disposition is then honoured, so `timeout` still reports 143).

### Opting a port in

The counters are compiled into the generated OBJECT library only when
`REX_GUEST_REACH` is defined for it. The generated `<pch>.h` selects the
prologue at preprocessing time, so the codegen output is identical whether or
not a port opts in. In the port's `CMakeLists.txt`, after
`rexglue_setup_target(<target>)`:

```cmake
option(REXGLUE_GUEST_REACH "Count calls to every generated guest function (REX_TRACE_REACH)" OFF)
if(REXGLUE_GUEST_REACH)
    target_compile_definitions(<target>_recomp PRIVATE REX_GUEST_REACH=1)
endif()
```

Then `cmake -DREXGLUE_GUEST_REACH=ON ...` and rebuild the recomp library.
Cost per call: one `lock xadd` on a per-function `.bss` slot plus a
never-taken branch (the slot is a constant-initialised static, no guard
variable); the first call of each function additionally registers the slot
with `rex::reach::Register`. Runtime API: `include/rex/system/reach_trace.h`
(`rex::reach::{Slot, Hit, Register, Dump, DumpTo, Enabled}`), implementation
`src/system/reach_trace.cpp`.

## symbols.json (frozen, Stage 0)

Produced by `tools/native_map.py <port> [--binary path] [--config linux]` at
`<port>/out/native/symbols.json`. Answers, from the linked image, which guest
functions are still the recompiled body, which the port displaced, and which
the linker dropped.

```json
{
 "format": "rex-native-symbols/1",
 "port": "/abs/path/to/pgr4-recomp",
 "binary": "/abs/path/to/pgr4-recomp/out/build/linux/projectgotham4",
 "counts": {"generated": 33790, "displaced": 0, "dropped": 0},
 "stale_partition_entries": 237,
 "extra_strong": [{"name": "sub_82000000", "defined_in": "CMakeFiles/x.dir/src/y.cpp.o"}],
 "symbols": {
  "823DDA50": {"name": "sub_823DDA50", "status": "generated", "defined_in": null, "partition": 41},
  "8269ADD8": {"name": "sub_8269ADD8", "status": "displaced", "defined_in": "CMakeFiles/projectgotham4.dir/src/d3d9native/gen/band_overrides.cpp.o", "partition": 75, "imp_present": true}
 }
}
```

* `symbols` keys: every address that has a `DEFINE_REX_FUNC(sub_X)` body in
  `<port>/generated/*/*_recomp.*.cpp`. The partition json is not the source
  of truth (it keeps addresses codegen no longer emits; their number is
  reported in `stale_partition_entries`).
* `status`:
  * `generated`: `sub_X` is a weak (`W`) symbol in the image, i.e. the alias
    to `__imp__sub_X` that codegen emitted is what runs.
  * `displaced`: `sub_X` is a strong (`T`) symbol in the image. `defined_in`
    is the port object file (relative to `out/build/<config>`) that defines
    it, or `null` when no port object does (then it came from a library).
    `imp_present` says whether `__imp__sub_X` survived the link (a hook that
    forwards to the original keeps it; a full replacement lets gc drop it).
  * `dropped`: neither `sub_X` nor `__imp__sub_X` is in the image (never
    referenced, removed by `--gc-sections`).
* `partition`: index of the generated `.cpp` that holds the body, or `null`.
* `extra_strong`: strong `sub_X` symbols in the image for addresses codegen
  never emitted (a port hooking an address outside the generated set).
* `counts` sums `symbols` by status.

The tool reads `nm --defined-only` of the binary and of every `.o` under
`out/build/<config>/CMakeFiles/*.dir/` except the `_recomp.dir` OBJECT
library. With no `--binary` it takes the largest executable in
`out/build/<config>`.

## Placeholders (owned by later workstreams)

### entrypoints.json (A1 discovery)

`PORT/docs/d3d/entrypoints.json` / `PORT/src/<lib>native/entrypoints.json`:
list of seam entry points. Proposed record, to be frozen by A1:

```json
{"addr": "8269ADD8", "name": "D3DDevice_DrawIndexedVertices", "arity": 6,
 "tier": 1, "callers": ["823DDA50"], "evidence": ["mnhash:ss", "trace:gpu_draw_trace"]}
```

Addresses use the same 8-hex-digit form as above so they join directly against
`symbols.json` and the reach CSV.

### native.toml (frozen, B1; emitted by B3)

`PORT/native.toml`: the modules a port is migrating, their address ranges and
the policy `tools/native_check.py` (and the CMake target `native-check`)
enforces. Read with `tomllib`; loader `native_check.load_native_toml`.

```toml
format = "rex-native-toml/1"

[module.d3d9]
description = "360 D3D9 runtime band"
ranges = ["82690000-8269FFFF"]        # inclusive, 8 hex digits; `0x` tolerated;
                                      # `range = "82690000-8269FFFF"` is accepted too
band = "docs/d3d/band.json"           # optional; when the file exists its ranges
                                      # replace `ranges` (A1 output, see below)
policy = "no-guest-code"              # no-guest-code | banned | reach-zero
banned = ["8269ADD8"]                 # policy = banned: only these addresses count
allow = ["82691000"]                  # never offenders (documented exceptions)
entrypoints = "docs/d3d/entrypoints.json"   # informational
```

* Module names are free identifiers (`d3d9`, `audio`, `kernel`, ...); the
  report adds a synthetic `all`. A module needs `ranges`/`range`/`band` or,
  for `banned`, the list.
* `band.json` (A1) is accepted in any of: `{"ranges": ["lo-hi", ...]}`,
  `{"range": "lo-hi"}`, `{"start": "lo", "end": "hi"}`, or a list of those.

**What counts as guest code.** Codegen emits `__imp__sub_X` (the body) and a
weak alias `sub_X`. A port's strong `sub_X` displaces the alias but the
`__imp__` body survives the link whenever the hook forwards to it, so the
presence of `T __imp__sub_X` proves nothing. The two checks are therefore:

* *Static* (from `symbols.json`): guest code is **reachable** in a module when
  any address in its ranges has status `generated` (weak `sub_X`, i.e. the
  recompiled body is what any caller, vtable included, resolves to).
  `displaced` and `dropped` pass.
* *Dynamic* (from a reach CSV): guest code was **reached** when any address in
  the ranges has `count > 0`. The counter is in the generated prologue of
  `__imp__sub_X`, so a forwarding hook still counts; a module is only native
  when its hooks stop forwarding. Both `REX_TRACE_REACH` output and
  `tools/reach_from_perf.sh` output (CPU samples in the same shape) are valid
  inputs.

| policy | static offenders | dynamic offenders (when a CSV is given) |
|---|---|---|
| `no-guest-code` | `generated` in ranges | reached in ranges |
| `banned` | `generated` among `banned` | reached among `banned` |
| `reach-zero` | (skipped) | reached in ranges |

Exit status: 0 pass, 1 offenders (printed with counts, hottest first), 2 bad
configuration. Without a CSV the dynamic check is reported as skipped;
`--require-reach` turns that into exit 2.

### Reach CSV from perf (B1)

`tools/reach_from_perf.sh record -- <cmd>` / `convert perf.data -o reach_perf.csv`
produce the reach CSV shape with `count` = CPU samples whose leaf frame is a
`sub_X`/`__imp__sub_X` symbol (`--inclusive`: every distinct guest frame in
the chain, once per sample). A trailing comment
`# samples=N guest_rows=R other_samples=O mode=self|inclusive` records the
process total so the guest share of CPU can be reported; readers skip rows
whose first field is not 8 hex digits, so the trailer is harmless to any
reach-CSV consumer. `native_report.py --perf` reads it as the CPU weight.

### guest_summary.json (B1, derived, not frozen)

`native_report.py --write` leaves `out/native/guest_summary.json` beside
`symbols.json`: `{"format": "rex-native-guest-summary/1", "reach": path|null,
"perf": path|null, "modules": {name: {functions, generated, displaced,
dropped, policy, reached?, reached_generated?, calls_total?, calls_generated?,
cpu_total?, cpu_generated?}}}`. `native_targets.py` ranks modules from it
(CPU share in generated code, else calls, else function share). It is a
cache of the report, regenerated on every `--write`.

## .snap (frozen, B2)

One file per recorded call of a hooked guest function, written by
`src/system/snapshot.cpp` (API `include/rex/system/snapshot.h`, layout
`include/rex/system/snapshot_format.h`) into `REX_SNAP_DIR` as
`<ADDR8>_<tag>_<seq>.snap` (`8269ADD8_sub_8269ADD8_0003.snap`). Replayed by
`tests/native/snap_replay`.

```
FileHeader   40 bytes  magic "REXSNAP\0", version=1, header_size=40, addr,
                       range_count, timestamp_ns (unix), tag_len, flags
tag          tag_len bytes, no terminator, zero-padded to a multiple of 4
RangeRecord  16 bytes x range_count: addr, size, stored, flags
RegFile      2608 bytes  entry registers
bytes        entry copy of every range, table order, `stored` bytes each
RegFile      2608 bytes  exit registers           only when flags & 1
bytes        exit copy of every range             only when flags & 1
```

* All integers are little-endian host order (the recorder and the replayer
  run on the same host class; the file is not an interchange format between
  architectures). Structs have no implicit padding: `RegFile` is
  `r[32] u64 | lr u64 | ctr u64 | xer so,ov,ca,pad u8 | cr[8] u8 (raw 4-bit
  field) | fpscr u32 | msr u32 | vscr_sat u8 | pad[11] | f[32] u64 (offset
  304) | v[128][16] (offset 560)`; `static_assert`s in the header pin it.
* `addr` is the guest address of the recorded function (the 8-hex-digit key
  used by every other file here is `%08X` of it). `tag` is the hook's label,
  normally `sub_<ADDR8>`.
* `flags & 1` (`kHasExit`): the exit half is present. A file without it is a
  call that never returned before the process ended; `snap_replay` refuses to
  replay it.
* `RangeRecord.stored < size` with `flags & 1` (`kTruncated`) means the range
  was cut at `REX_SNAP_MAX_BYTES` (default 1 MiB).
* Ranges are guest virtual addresses declared by the hook, evaluated at entry
  with the argument registers in scope. Ranges with address 0 or size 0 are
  dropped, so `{ctx.r5.u32, 64}` is safe when r5 may be null.
* Per-address cap `REX_SNAP_MAX` (default 32); `seq` counts from 0 per
  address. Recording is not enabled without `REX_SNAP_DIR`.

**Dirty tracking limitation.** Both copies of every declared range are stored
and the reader diffs them; the runtime's write-watch state is *not* consulted
(it belongs to the GPU uploader and toggling it from a CPU hook changes the
residency behaviour under test). Writes outside the declared ranges are
invisible to a replay: declare every output the function is known to touch
(argument pointers, the object at r3, palette/ring buffers). Undeclared side
effects are what tee digests catch.

### Hooking a function for recording

In a port TU:

```cpp
#include <rex/system/snapshot.h>
REX_SNAP_HOOK(sub_8269ADD8, {ctx.r4.u32, 64}, {ctx.r5.u32, ctx.r6.u32 * 4})
REX_SNAP_HOOK(sub_82695BE0)   // registers only
```

This defines the strong `sub_X`, records, calls `__imp__sub_X`, records again.
Hooks that already dispatch on a mode (PGR4's generated `band_overrides.cpp`)
call the two-phase API directly around the `__imp__` call:
`Call* c = rex::snapshot::Record(tag, addr, ctx, base, ranges); ...;
rex::snapshot::Record(c, ctx, base);`.

### Replay tolerances (`native.toml`, `[snap]` tables)

```toml
[snap.default]
float_ulp = 0
[snap."8269ADD8"]                     # 8 upper-case hex digits, quoted
float_ulp = 2                         # f1 / fN and float_ranges, in ULPs
ignore_ranges = ["0A70462C-0A70463F", "0A73A8B0+10"]   # hex LO-HI or BASE+LEN
float_ranges  = ["0A704000-0A73A8AF"] # aligned 4-byte slots compared as BE f32
compare_regs  = ["r3", "f1"]          # default; also rN vN crN lr ctr xer
ignore_regs   = []
r_bits        = 32                    # compare the low 32 bits of rN (or 64)
stack_bytes   = 65536                 # zero-filled window committed below r1
```

`[snap.default]` is applied first, then the address table. `snap_replay` maps
a private 4 GiB reservation so `base + u32` is valid for any guest address,
commits and fills the recorded ranges, restores the entry registers, calls the
replacement and reports every register and byte that differs from the recorded
exit (`want`/`got`/`entry`, and whether the recorded call or the replacement
wrote there). Invocation: `snap_replay f.snap --plugin lib.so [--symbol
sub_X] [--toml native.toml]`; `--identity` replays the recorded exit against
itself (loader/differ self-check); `--dump` prints the recording. Exit 0 match,
1 mismatch, 2 usage/load error. Identity test: `tests/native/snap_replay_test`.

## Tee digest (frozen, B2)

JSON lines written by `rex::tee::Channel` (`include/rex/system/tee.h`,
`src/system/tee.cpp`) to `REX_TEE_LOG`, compared by `tools/tee_diff.py`.

```
{"frame":412,"key":"d3d/draw","hash":"3f9c1a0e77d2b4c1","n":1873}
```

* One object per line, keys in this order, no other fields.
* `frame`: decimal, the shared frame counter (`rex::tee::EndFrame()` from the
  presenting hook publishes every channel and advances it). Frames with no
  digests produce no lines.
* `key`: `<channel>/<key>`; channel and key are short ASCII names without
  `/`, `"` or whitespace. Lines for one channel and frame are sorted by key.
* `hash`: 16 lower-case hex digits, XXH3-64 of every byte sequence passed to
  `Digest(frame, key, bytes, len)` for that frame and key, in call order
  (order-sensitive by design: draw order is part of what is checked).
* `n`: decimal number of `Digest` calls folded into the hash.

`tools/tee_diff.py a.jsonl b.jsonl [--all] [--keys REGEX] [--offset N]
[--frames LO-HI] [--json out.json]` compares the frames present in both logs
in ascending order, keys sorted, and prints the first `(frame, key)` whose hash
differs or is missing on one side; exit 0 match, 1 divergence, 2 bad input.
`tools/ab_run.sh <port> <secs> "<script>" "<envA>" "<envB>"` drives two
`headless_play.sh` runs with `REX_TEE_LOG` set, then prints one table with the
tee result, the port's `tools/d3d/ab_frames.sh` output when present, and the
`measure.sh` per-frame medians from each run's frame-stats CSV.

## rexnative (B3): files the tool reads and writes

`tools/rexnative` is the estate-wide driver for the formats above. Stages and
their artefacts (PORT-relative unless absolute):

| stage | reads | writes |
|---|---|---|
| `ingest PORT` | `generated/*/<title>_recomp.*.cpp` (or XenonRecomp `ppc_recomp.*.cpp`) | `out/rexnative/ingest.json`, `out/rexnative/graph.json` (call-graph cache) |
| `harvest INGEST --lib L [--reach csv]` | ingest, reach trace CSV | `out/rexnative/harvest_L.json`; `--emit-counters` also the legacy counter TU `out/rexnative/harvest_L.cpp` |
| `emit INGEST --lib L [--dry-run]` | harvest, reach CSV, existing `src/Lnative/api_table.toml`, `native.toml`, optional `docs/<L>/entrypoints.json` and `arity.json` (A1) | `native.toml` (merged), `src/Lnative/{entrypoints.json, api_table.toml, gen/overrides.cpp, gen/record.cpp}` (regenerated) and `src/Lnative/{L_native.h, L_native.cpp, README.md}` (scaffold, written once) |
| `link PORT --lib L` | emit output | `src/Lnative/sources.cmake`; inserts the `include()` only after a `# rexnative:link` marker line in `CMakeLists.txt` |
| `verify PORT --lib L` | `tools/native_check.py`, `tests/native/snap_replay`, `tools/tee_diff.py` (each only if present) | `src/Lnative/VERIFY.md` (or `NATIVE_COVERAGE.md` once `native_report.py --append` exists) |
| `status PORT` | `native.toml`, `out/native/symbols.json`, reach CSV | nothing |

Nothing under `generated/` is ever written. `L` is the library key (`d3d9`,
`audio`); the port directory is `src/<L>native/`, the C++ namespace `<L>native`,
the cvars `<title>_<L>`, `<title>_<L>_entries`, `<title>_<L>_log_counts`.

### harvest_<lib>.json (`rex-native-harvest/1`)

```json
{"format": "rex-native-harvest/1", "lib": "d3d9", "title": "projectgotham4",
 "region": "8268DBE0-826AFC40", "seed_span": "82695BE0-826A7C40", "seeds": 17,
 "frames": 301, "reach": "/abs/reach.csv",
 "summary": {"in_region": 612, "api_candidates": 230, "reached": 140, "reached_api_candidates": 61,
             "calls_total": 1234567, "ring_candidates": 11},
 "functions": [{"addr": "82695BE0", "count": 301, "band": "per-frame", "entry": true,
                "callers_outside": ["82293828"], "callers_outside_count": 2,
                "imports": ["VdSwap"], "reaches_ring_import": true, "roles": ["present"]}]}
```

* `region` is the instrumentation bound (seed span widened by `--pad`, default
  0x8000, or `--range`); it is deliberately generous and is NOT the module range.
* `count` comes from the reach CSV (absent function = 0); `band` is
  `per-draw | per-frame | setup | once | unreached`, computed per frame when
  `frames` (the reach count of the `present` anchor) is known, else by absolute
  thresholds 1000/100/10; `unmeasured` when no CSV was given.
* `entry`: has at least one caller outside `region` (public-API candidate).
* `reaches_ring_import`: static, transitive within the region, to a function
  that calls one of `VdSwap, VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack,
  VdGetSystemCommandBuffer, VdSetSystemCommandBufferGpuIdentifierAddress,
  VdInitializeEngines, VdInitializeEDRAM, VdShutdownEngines`. An
  under-approximation: PM4 writers use ordinary stores. Checksum attribution
  (`--attribute-ring --emit-counters`) needs the counter TU around each call and
  cannot be derived from a CSV of counts.
* `roles`: `present` = direct caller of `VdSwap`, `ring_init` = direct caller of
  `VdInitializeRingBuffer` (d3d9 only).

### src/<lib>native/entrypoints.json (`rex-native-entrypoints/1`)

Superset of the A1 record proposed above, so `hookgen.py --entrypoints` and
`rexnative emit --entrypoints` both accept it:

```json
{"format": "rex-native-entrypoints/1", "title": "projectgotham4", "lib": "d3d9",
 "region": "8268DBE0-826AFC40", "reach": "/abs/reach.csv", "frames": 301, "measured": true,
 "entries": [{"addr": "82695BE0", "name": "Present", "arity": 4, "tier": 1, "role": "present",
              "count": 301, "band": "per-frame", "callers": ["82293828", "828382B0"],
              "evidence": ["static:callers-outside-region", "reach:reach.csv", "static:reaches-ring-import"],
              "imports": ["VdSwap"]}]}
```

* `tier`: 1 = called from outside the region and reached (or unmeasured);
  2 = internal but hooked (role anchor, `--include-internal`, `--pick`,
  or kept from an existing table); 3 = in the table but not reached.
* `evidence` tokens: `static:callers-outside-region`, `static:internal`,
  `static:reaches-ring-import`, `reach:<csv>`, `entrypoints:<json>`, `pick`,
  `poll-loop` (count more than 50x the next entry: a poll, not an API rate).
* `callers` is capped at 32 addresses; `callers_outside_count` in the harvest
  json has the full number.
* Selection: entries lie inside the module `ranges` of `native.toml` (role
  anchors excepted); `--min-count N` drops tier-1 entries below N calls.

### src/<lib>native/api_table.toml

Same schema as the hand-built PGR4 table (`address, name, arity, mode, role,
note`) plus `freq` (regenerated) and `[meta] lib, region, generator`. Emit
re-reads the existing table and preserves `name` (unless still `sub_X`),
`arity`, `mode`, `role` and `note` per address, so hand edits and A1 renames
survive regeneration. `mode` defaults to `guest` for every new entry.

### native.toml as written by emit

```toml
format = "rex-native-toml/1"

[module.d3d9]
ranges = ["82690000-8269FFFF"]     # --range, else the existing table, else the 64 KiB hull of the seeds
policy = "no-guest-code"           # --policy
banned = []                        # entries whose table mode is "fatal"
entrypoints = "src/d3d9native/entrypoints.json"
api_table = "src/d3d9native/api_table.toml"
sources = "src/d3d9native/sources.cmake"
```

An existing `[module.<lib>]` table is merged: every key it already has is
kept verbatim (B1's `description`, A1's `band`/`entrypoints` pointers), only
missing keys are appended. Other tables are untouched.

### gen/overrides.cpp and gen/record.cpp

`gen/overrides.cpp` has the same shape as `pgr4-recomp/tools/d3d/hookgen.py`
output: one `REX_HOOK_RAW(sub_X)` per entry, `detail::Hit`, a switch on
`EffectiveMode` (`guest` forwards to `__imp__sub_X`; `tee` brackets it with
weak `hooks::TeePre_X`/`TeePost_X`; `native` calls weak `hooks::Native_X` or
is fatal; `fatal` is fatal), role calls `OnPresent`/`OnRingInit`, and the
measured frequency in the comment above each override. The only differences
from the hand-built PGR4 file are the include (`<lib>native/<lib>_native.h`
instead of `mode.h`) and the table order (by address).

`gen/record.cpp` is one `REX_SNAP_HOOK(sub_X)` per entry (registers only;
per-entry ranges via `<lib>native/snap_ranges.h` macros
`<LIB>_SNAP_RANGES_<ADDR>`). Because `REX_SNAP_HOOK` also defines the strong
`sub_X`, record.cpp and overrides.cpp are mutually exclusive:
`sources.cmake` builds record.cpp only when `<TITLE>_<LIB>NATIVE_RECORD=ON`.
When the SDK has no `REX_SNAP_HOOK` the file carries a plain forwarding
placeholder and says so in its header.

### sources.cmake

Included after `rexglue_setup_target(<title>)`; `target_sources(<title> PRIVATE
...)` with `<lib>_native.cpp` plus overrides.cpp or record.cpp, adds `src/` to
the include path, defines the `REXGLUE_GUEST_REACH` option (if the port has
not) and applies `REX_GUEST_REACH=1` to `<title>_recomp` when it is ON.

### VERIFY.md block

Appended per `rexnative verify` run:

```
## rexnative verify: <title> / <lib> - PASS|FAIL|INCOMPLETE (YYYY-MM-DD HH:MM)

| gate | result | detail |
| native.toml | ok | ... |
| native_check | PASS/FAIL/SKIPPED | last lines of tools/native_check.py |
| snap_replay | ... | |
| tee_diff | ... | |
| coverage | ok | the `status` coverage line |
```

A gate whose tool is absent is `SKIPPED`, never `PASS`; overall is `FAIL` if
any gate failed, `PASS` if at least one gate passed and none failed, else
`INCOMPLETE`.
