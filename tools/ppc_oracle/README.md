# PPC test oracle

Generates `tests/ppc/asm/instr_*.s` cases whose `REGISTER_OUT` / `MEMORY_OUT`
values are produced by executing the instruction on a real PowerPC, rather than
being derived by hand.

Each case is assembled with the bundled `tools/binutils` toolchain, linked into
a freestanding static ELF, and run under qemu user-mode. The register file is
dumped afterwards and rendered in the test format the suite already uses.

## Requirements

- `qemu-ppc-static` and `qemu-ppc64-static` on `PATH`
- `tools/binutils` (already vendored)

## Files

| File | Purpose |
| --- | --- |
| `gen_vmx_tests.py` | 32-bit AltiVec harness (`QEMU_CPU=7400`); vector register in/out |
| `gen_ppc_tests.py` | 64-bit harness (`QEMU_CPU=power7`); GPR/FPR/CR/XER plus a scratch memory block |
| `vmx_cases.py` | Case tables for the integer VMX instructions |
| `scalar_cases.py` | Case tables for the scalar instructions |

Both harnesses have a self-test that reproduces an expectation already checked
into the suite, which is the cheapest way to confirm the oracle itself is sound:

```sh
python3 gen_vmx_tests.py selftest   # reproduces instr_vavguh.s
python3 gen_ppc_tests.py            # cntlzw, fabs and a store to scratch
```

## Regenerating

```sh
python3 vmx_cases.py
python3 scalar_cases.py
```

Then re-run CMake configure: `tests/ppc/CMakeLists.txt` globs the assembly
directory without `CONFIGURE_DEPENDS`, so new files are invisible to an
incremental build until the project is reconfigured.

## Notes

- The 64-bit harness reserves `r31` as its base pointer and re-materialises it
  after the instruction under test, so `r31` is never observable in a test.
- `ORACLE_CPU` overrides the qemu CPU model for the 64-bit harness.
- `isel`, `fcpsgn` and `lfiwax` generators exist but are not part of the default
  run: ReXGlue's disassembler uses a Xenon dialect mask that excludes `PPCISEL`
  and `POWER6`, so those encodings never reach a builder.
