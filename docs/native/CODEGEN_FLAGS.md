# Codegen flags (Programme B, workstream B4)

Opt-in code generation options. None of them changes the generated output of a
port that does not set them: the default output is byte-identical to the
pre-B4 tool (proved on the `geometrywars` scratch copy, see "Byte identity"
below). Each section gives the manifest key, what the output looks like, the
caveats, and the A/B procedure that decides whether a title may keep it.

| Flag | Where | Effect |
|---|---|---|
| `[entrypoint.signatures]` | manifest table | emits `R <name>_typed(uint8_t* base, Args...)` beside the raw body, plus a declaration and `<name>_sig_t` in `<project>_funcs.h` |
| `non_volatile_as_local` in `[entrypoint.functions]` | per function | tri-state override of the module-wide `non_volatile_as_local` |
| `nonvolatile_memory = true` | `[entrypoint]` / module | `REX_LOAD_*`/`REX_STORE_*` lose `volatile`; MMIO macros keep it |
| `--codegen-flag` on `rexglue recompile-tests`, `REXGLUE_PPC_TEST_VARIANTS` | test build | same instruction tests built once per flag set |

The runtime build already accepts `-DREX_GUEST_REACH` (Stage 0) for reach
counting; it composes with everything here.

## 1. Typed signatures

```toml
[entrypoint.signatures]
0x82021EF0 = "float(void*)"
0x8201A2C0 = "uint32(uint32, be<uint16>, MappedPtr<uint8_t>, double)"
```

Grammar: `R(Args...)`, with each type one of

| spelling | wrapper parameter | PPC ABI |
|---|---|---|
| `void` | (return only) | nothing read back |
| `int32` `uint32` `int64` `uint64` | `int32_t` ... `uint64_t` | next of r3..r10, `u64` |
| `float` `double` | `float` / `double` | next of f1..f13, `f64` |
| `T*` (any identifier) | `uint32_t` guest address | next of r3..r10 |
| `be<T>` | `rex::be<T>` | next of r3..r10 via `operator T()` |
| `MappedPtr<T>` | `rex::MappedPtr<T>` | next of r3..r10 via `guest_address()`; a returned one is rebuilt with `rex::memory::GuestPtr` |

Integer-class and float-class ordinals are counted separately, the same rule
`rex::ppc::GatherFunctionArgumentsFromTypes` applies to `REX_IMPORT`. At most 8
integer-class and 13 float parameters; more is a config error (no stack
arguments yet). Parse errors name the address and the offending type and skip
that entry; codegen continues.

Output, in the recomp file that holds the body:

```cpp
// Typed wrapper: [entrypoint.signatures] 0x82021EF0 = "float(void*)"
float sub_82021EF0_typed(uint8_t* base, uint32_t a0) {
	rex::CallFrame frame(*rex::runtime::current_ppc_context());
	PPCContext& ctx = frame.ctx;
	ctx.r3.u64 = a0;
#if defined(REX_VERIFY_TYPED)
	PPCContext verify = ctx;
	(void)rex::ppc::ImportFunction<float(uint32_t)>{sub_82021EF0}(verify, base, a0);
#endif
	sub_82021EF0(ctx, base);
#if defined(REX_VERIFY_TYPED)
	if (verify.r3.u64 != ctx.r3.u64 || verify.f1.u64 != ctx.f1.u64) {
		REXLOG_ERROR("REX_VERIFY_TYPED sub_82021EF0_typed: r3 {:#x}/{:#x} f1 {}/{}", ...);
	}
#endif
	return static_cast<float>(ctx.f1.f64);
}
```

and in `<project>_funcs.h`:

```cpp
float sub_82021EF0_typed(uint8_t* base, uint32_t a0);
using sub_82021EF0_sig_t = float(uint32_t);   // for rex::ppc::ImportFunction / REX_IMPORT
```

The wrapper takes the calling thread's context from the bound `ThreadState`
(as hooks do) and runs the body in a `rex::CallFrame`, so the caller's
registers other than `fpscr` are untouched. When signatures exist the PCH adds
`<rex/hook.h>`, `<rex/system/thread_state.h>` and `<rex/system/xmemory.h>`; a
port without them gets the old PCH.

`-DREX_VERIFY_TYPED` (compile definition on the `<target>_recomp` object
library) also runs the call through `rex::ppc::ImportFunction`, the SDK's
independent marshaller, on a copy of the frame, and logs an error when r3 or
f1 differ. This executes the body twice; enable it only for pure accessors or
in a throwaway run, never in a build that ships.

Unit test: `/home/jon/pgr4dump/gw-scratch/tests/typed_signature_test.cpp`
(`-DGEOMETRYWARS_TYPED_TEST=ON`, target `typed_signature_test`), which builds
the accessor's object graph in guest memory and checks the wrapper against the
raw body and against `ImportFunction<sub_82021EF0_sig_t>`. It passed in both
plain and `REX_VERIFY_TYPED` builds. Standalone (no port build needed):

```sh
SDK=~/rexglue-vmx; PORT=~/pgr4dump/gw-scratch
FL="-std=c++23 -O1 -w -I $SDK/include -I $SDK/thirdparty/fmt/include -I $SDK/thirdparty/simde \
    -I $SDK/thirdparty/spdlog/include -I $SDK/thirdparty/tomlplusplus/include -I $PORT/generated/default \
    -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -msse4.1 -mssse3 -fno-strict-aliasing"
for f in $PORT/generated/default/*.cpp $PORT/tests/typed_signature_test.cpp; do clang++ $FL -c $f -o $(basename $f).o & done; wait
clang++ -o typed_signature_test *.o -L $SDK/out/linux-amd64/Release -lrexruntime -Wl,-rpath,$SDK/out/linux-amd64/Release
./typed_signature_test
```

A/B: the wrapper never changes the raw body, so the only question is whether
the signature is right. Build with `-DREX_VERIFY_TYPED`, drive the seam that
calls `<name>_typed` (headless capture, below) and grep the log for
`REX_VERIFY_TYPED`; zero lines is the pass.

## 2. Per-function non-volatile register lifting

```toml
[entrypoint]
non_volatile_as_local = true          # module default (unchanged behaviour)

[entrypoint.functions]
0x82021308 = { non_volatile_as_local = false }               # keep this one in ctx
0x8205A580 = { size = 0x1C0, non_volatile_as_local = true }  # localize under a module that does not
```

`FunctionConfig::nonVolatileAsLocal` is `std::optional<bool>`: absent inherits
the module flag, which is what every existing manifest does.
`BuilderContext::localizeNonVolatiles()` consults it where the module flag was
read before; `share_registers` still wins (a funclet never owns its registers).

Diff for the `false` override on `sub_82021308` (module `true`):

```diff
 DEFINE_REX_FUNC(sub_82021308) {
 	REX_FUNC_PROLOGUE();
-	PPCRegister r31{};
 	uint32_t ea{};
 ...
-	REX_STORE_U64(ctx.r1.u32 + -16, r31.u64);
+	REX_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64);
 ...
-	r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
+	ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
```

Caveats, both inherited from how the module flag works:

* `__savegprlr_N`/`__restgprlr_N` (and the fpr/vmx helpers) are elided in a
  function that localizes, whether by the module flag or by the override. Under
  a module that localizes, the helper bodies themselves are no-ops, so a
  function forced to `false` there cannot rely on them to preserve ctx
  registers across its own calls; its callers use locals, so nothing observes
  that, but it is not "the old codegen" for that one function.
* A `[functions]` entry without `size`/`end` registers a CONFIG function at
  that address with discovery deciding the extent (the same as an entry that
  only sets `share_registers`). Give a size when the boundary matters.

A/B: `tools/ppc_oracle/ab_flags.sh default non_volatile_as_local` for the
instruction level (see 4); at title level, flip one function at a time and
compare reach counts and the tee digest with `ab_run.sh` (below).

## 3. Non-volatile plain memory

```toml
[entrypoint]
nonvolatile_memory = true
```

The generated PCH then defines `REX_CONFIG_NONVOLATILE_MEMORY` and emits

```c
#define REX_LOAD_U32(x) __builtin_bswap32(*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_STORE_U32(x, y) (*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
```

(and the U8/U16/U64 forms) instead of the `volatile` ones. `REX_MM_LOAD_*` /
`REX_MM_STORE_*`, which the builders select for MMIO-base registers, keep
`volatile` and the handler dispatch. Nothing else in the PCH moves. Without the
key the PCH is byte-identical to before.

What it buys: the host compiler may keep a guest value in a register across
instructions, combine adjacent loads and stores, and hoist loads out of loops.
What it costs: every place the title relies on a plain load observing another
thread's store without a barrier (spin waits on a flag, producer/consumer
rings without `lwsync`, the kind of pattern PGR4's crowd palette turned out to
be) can now spin forever or read stale data. `sync`/`lwsync`/`eieio` still
emit their fences, and `lwarx`/`stwcx.` are unaffected.

A/B, in this order:

1. `tools/ppc_oracle/ab_flags.sh default nonvolatile_memory` must show zero
   differing tests (memory tests set and read through `REX_STORE`/`REX_LOAD`,
   so any miscompile shows up here first).
2. Title level: two manifests (or two `template_dir`-free copies of the port)
   differing only in this key, both built, run through the same headless
   script; `ab_run.sh` must report identical reach counts and tee digests, and
   frame captures must match to the tolerance the port already uses.
3. Soak: a longer headless run watching for a hang (a spin on a plain load is
   the failure mode) before it is left on for a title.

## 4. A/B protocol

### Instruction level (qemu oracle)

`tests/ppc/asm/*.s` carry `REGISTER_OUT`/`MEMORY_OUT` values produced on a real
PowerPC by `tools/ppc_oracle`. `rexglue recompile-tests` now takes
`--codegen-flag <name>` (repeatable; `non_volatile_as_local`,
`nonvolatile_memory`, `ctr_as_local`, `xer_as_local`, `cr_as_local`,
`non_argument_as_local`), and `tests/ppc/CMakeLists.txt` builds one executable
per entry of `REXGLUE_PPC_TEST_VARIANTS` (`;`-separated, flags joined by `+`;
`default` is the shipped codegen and keeps the name `ppc_tests`, a variant is
`ppc_tests_<flags>` with `+` replaced by `_`, ctest prefix
`ppc.<flags>.`). `tools/ppc_oracle/ab_flags.sh` configures a build dir with
the matrix, builds the executables, runs every test case in its own process,
and writes `<build>/ppc_ab/<variant>.txt` and `summary.md`:

```sh
tools/ppc_oracle/ab_flags.sh                       # default, nvlocal, nvmem, both
tools/ppc_oracle/ab_flags.sh -B out/build/ab default nonvolatile_memory
```

Reading the table: a flag passes when its column equals the default column.
`non_volatile_as_local` is expected to differ on the five files that feed
r14-r30 through `REGISTER_IN` (`instr_add`, `instr_and`, `instr_andc`,
`instr_lmw`, `instr_seq`): the harness writes `ctx.r25`, a localized body
reads its own zero-initialised `r25`. That is the flag's contract (a caller
hands non-volatiles over through the save area, never through ctx), not a
miscompile. Anything else that differs is a bug in the flag.

Result on 2026-09-24 (1571 test cases, clang, Release):

| variant | pass | fail | differs from default |
|---|---|---|---|
| default | 1571 | 0 | 0 |
| non_volatile_as_local | 1526 | 45 | 90 (the 45 REGISTER_IN cases above: 14 add, 14 and, 14 andc, 1 lmw, 2 seq) |
| nonvolatile_memory | 1571 | 0 | 0 |
| non_volatile_as_local+nonvolatile_memory | 1526 | 45 | 90 (identical set to non_volatile_as_local alone) |

Note: the executables land in `$<TARGET_FILE_DIR:rexruntime>`
(`out/linux-amd64/<config>/`), not under the build dir, which is where
`ab_flags.sh` looks first.

### Title level (`ab_run.sh`, headless captures)

`tools/ab_run.sh` (B2) runs one port twice through its `tools/headless_play.sh`
and prints one table (tee digest divergence, frame glitch/PSNR stats, per-frame
cost medians):

```
tools/ab_run.sh <port_dir> <secs> "<key script>" "<envA>" "<envB>" [labelA] [labelB]
```

The two sides differ by environment (`GAME_ENV`) and cvars (`AB_ARGS_A` /
`AB_ARGS_B`), so a codegen flag, which changes the binary, needs two builds of
the same port and an env word that selects between them:

1. Generate twice, never against the real manifest. Copy the port (or add a
   second `[entrypoint]`-equivalent manifest with its own `out_directory_path`
   and `template_dir`-free copy of the sources), set the flag in the copy only,
   `rexglue codegen` in both, build both with the generated OBJECT library
   compiled `-DREX_GUEST_REACH=1` (and `-DREX_VERIFY_TYPED` for signatures).
2. `headless_play.sh` runs `./<title>` from its own port's `out/build/linux`,
   and `AB_HEADLESS` is one script for both sides, so a binary A/B is two
   `ab_run.sh` invocations, one per port copy, with identical A and B sides
   (the second side is a free repeatability check), then a cross-port diff:

   ```sh
   KEYS="25:Return 100:e:35"
   (cd ~/ports/pgr4          && AB_OUT=out/ab-default tools/ab_run.sh . 60 "$KEYS" "REX_TRACE_REACH=$PWD/out/ab-default/reach.csv" "" default default)
   (cd ~/ports/pgr4-nvmem    && AB_OUT=out/ab-nvmem   tools/ab_run.sh . 60 "$KEYS" "REX_TRACE_REACH=$PWD/out/ab-nvmem/reach.csv"   "" nvmem nvmem)
   tools/tee_diff.py ~/ports/pgr4/out/ab-default/default/tee.jsonl ~/ports/pgr4-nvmem/out/ab-nvmem/nvmem/tee.jsonl
   diff <(sort ~/ports/pgr4/out/ab-default/reach.csv) <(sort ~/ports/pgr4-nvmem/out/ab-nvmem/reach.csv)
   ```

3. The gate from the plan: `tee_diff.py` reports no divergence, the reach
   diff is empty, frame stats in the two `ab_run.md` tables agree within the
   port's tolerance, and the oracle tests are green. Attach both `ab_run.md`
   files and the reach diff to the PR.

## Byte identity

Baseline and proof, on the scratch copy `/home/jon/pgr4dump/gw-scratch`
(`non_volatile_as_local = true`, longjmp/setjmp set, no B4 keys):

```sh
cmake --build ~/rexglue-vmx/out/build/linux-amd64 -j8 --target rexglue
cd ~/pgr4dump/gw-scratch && ~/rexglue-vmx/out/linux-amd64/Release/rexglue codegen geometrywars_manifest.toml --ignore-stamp
diff -rq -x '*.stamp' <baseline copy of generated/default> generated/default   # no output
```

After all B4 changes: 39 files unchanged, `diff -rq` clean except the two
`codegen.*.stamp` files (they embed the tool fingerprint). Enabling one key at
a time changed only the files the section above shows: signatures ->
`<project>_funcs.h`, `<project>_pch.h`, the one recomp file; per-function
override -> the one recomp file; `nonvolatile_memory` -> `<project>_pch.h`.
The test PCH (`resources/templates/test/ppc_config_h.inja`) renders identically
to HEAD without `--codegen-flag`.
