#!/usr/bin/env python3
"""Generate tests/ppc/asm/*.s cases with expected values taken from a real
PowerPC AltiVec oracle.

Each case is assembled with the bundled powerpc-none-elf binutils, linked into
a freestanding static ELF, and executed under qemu-ppc (QEMU_CPU=7400, a G4
with AltiVec).  The vector register file is dumped after the instruction under
test, so REGISTER_OUT values are observed hardware semantics, not hand math.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINUTILS = os.path.join(ROOT, "tools", "binutils")
AS = os.path.join(BINUTILS, "powerpc-none-elf-as")
LD = os.path.join(BINUTILS, "powerpc-none-elf-ld")
ASM_DIR = os.path.join(ROOT, "tests", "ppc", "asm")
QEMU = "qemu-ppc-static"

NUM_VREGS = 32


def _words(v):
    """Accept [w0, w1, w2, w3] ints or a 32-char hex string."""
    if isinstance(v, str):
        h = v.replace(" ", "").replace(",", "")
        return [int(h[i : i + 8], 16) for i in range(0, 32, 8)]
    return list(v)


def run_case(insn, inputs):
    """Execute one instruction under qemu and return {vreg: [4 words]}.

    inputs: {"v3": [w0..w3], ...} in PowerPC (big-endian) element order.
    """
    lines = [
        "  .text",
        "  .globl _start",
        "_start:",
        "  lis   3, invec@ha",
        "  addi  3, 3, invec@l",
    ]

    regs = sorted(int(r[1:]) for r in inputs)
    for slot, rn in enumerate(regs):
        lines.append(f"  li    4, {slot * 16}")
        lines.append(f"  lvx   {rn}, 4, 3")

    lines.append(f"  {insn}")

    lines += [
        "  lis   3, outvec@ha",
        "  addi  3, 3, outvec@l",
    ]
    for rn in range(NUM_VREGS):
        lines.append(f"  li    4, {rn * 16}")
        lines.append(f"  stvx  {rn}, 4, 3")

    lines += [
        "  li    0, 4",           # __NR_write
        "  li    3, 1",           # fd 1
        "  lis   4, outvec@ha",
        "  addi  4, 4, outvec@l",
        f"  li    5, {NUM_VREGS * 16}",
        "  sc",
        "  li    0, 1",           # __NR_exit
        "  li    3, 0",
        "  sc",
        "  .data",
        "  .align 4",
        "invec:",
    ]
    for rn in regs:
        w = _words(inputs[f"v{rn}"])
        lines.append("  .long " + ", ".join(f"0x{x:08X}" for x in w))
    lines.append("outvec:")
    lines.append(f"  .space {NUM_VREGS * 16}")

    src = "\n".join(lines) + "\n"

    with tempfile.TemporaryDirectory() as td:
        s = os.path.join(td, "t.s")
        o = os.path.join(td, "t.o")
        elf = os.path.join(td, "t.elf")
        with open(s, "w") as fh:
            fh.write(src)
        subprocess.run([AS, "-mregnames", "-maltivec", "-o", o, s], check=True)
        subprocess.run([LD, "-Ttext=0x10000000", "-o", elf, o], check=True)
        env = dict(os.environ, QEMU_CPU="7400")
        raw = subprocess.run([QEMU, elf], check=True, capture_output=True, env=env).stdout

    if len(raw) != NUM_VREGS * 16:
        raise RuntimeError(f"oracle produced {len(raw)} bytes for '{insn}'")

    out = {}
    for rn in range(NUM_VREGS):
        chunk = raw[rn * 16 : rn * 16 + 16]
        out[f"v{rn}"] = [int.from_bytes(chunk[i : i + 4], "big") for i in range(0, 16, 4)]
    return out


def fmt_vec(words):
    return "[" + ", ".join(f"{w:08X}" for w in words) + "]"


def emit(mnemonic, cases, out_dir=ASM_DIR):
    """cases: list of (insn_text, {vreg: words}, [reported_out_regs])."""
    blocks = []
    for idx, (insn, inputs, reported) in enumerate(cases, 1):
        res = run_case(insn, inputs)
        body = [f"test_{mnemonic}_{idx}:"]
        for r in sorted(inputs, key=lambda x: int(x[1:])):
            body.append(f"  #_ REGISTER_IN {r} {fmt_vec(_words(inputs[r]))}")
        body.append(f"  {insn}")
        body.append("  blr")
        for r in reported:
            body.append(f"  #_ REGISTER_OUT {r} {fmt_vec(res[r])}")
        blocks.append("\n".join(body))

    path = os.path.join(out_dir, f"instr_{mnemonic}.s")
    with open(path, "w") as fh:
        fh.write("\n\n".join(blocks) + "\n")
    print(f"wrote {path} ({len(cases)} cases)")


def self_test():
    """Reproduce an existing checked-in expectation to prove the oracle."""
    res = run_case(
        "vavguh 5, 3, 4",
        {"v3": [0x00000001, 0x00020003, 0x00040005, 0x00060007],
         "v4": [0x00080009, 0x0000000A * 0 + 0x000A000B, 0x000C000D, 0x000E000F]},
    )
    expect = [0x00040005, 0x00060007, 0x00080009, 0x000A000B]
    assert res["v5"] == expect, (res["v5"], expect)
    print("oracle self-test OK (vavguh matches checked-in instr_vavguh.s)")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        self_test()
