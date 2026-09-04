#!/usr/bin/env python3
"""64-bit PowerPC oracle for scalar instruction tests.

Assembles a freestanding ELF64 (ELFv1, big-endian) with the bundled
powerpc-none-elf binutils and runs it under qemu-ppc64, capturing the GPR,
FPR, CR, XER and scratch-memory state after the instruction under test.

r31 is reserved as the harness base pointer, so tests must not use it.
"""

import os
import struct
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINUTILS = os.path.join(ROOT, "tools", "binutils")
AS = os.path.join(BINUTILS, "powerpc-none-elf-as")
LD = os.path.join(BINUTILS, "powerpc-none-elf-ld")
ASM_DIR = os.path.join(ROOT, "tests", "ppc", "asm")
QEMU = "qemu-ppc64-static"

BUF_ADDR = 0x10001200          # guest address of the harness buffer
OFF_GPR_IN = 0x000
OFF_FPR_IN = 0x100
OFF_CR_IN = 0x200
OFF_XER_IN = 0x204
OFF_SCRATCH = 0x280            # guest-visible scratch area for load/store tests
SCRATCH_SIZE = 0x80
OFF_GPR_OUT = 0x300
OFF_FPR_OUT = 0x400
OFF_CR_OUT = 0x500
OFF_XER_OUT = 0x504
DUMP_SIZE = 0x508

SCRATCH_ADDR = BUF_ADDR + OFF_SCRATCH
BASE_REG = 31


def run_case(insn, gprs=None, fprs=None, cr=0, xer=0, scratch=None):
    """Execute one instruction and return the resulting machine state.

    gprs:    {index: 64-bit value}, index != 31
    fprs:    {index: 64-bit raw double bits}
    scratch: bytes placed at SCRATCH_ADDR before execution
    """
    gprs = dict(gprs or {})
    fprs = dict(fprs or {})
    if BASE_REG in gprs:
        raise ValueError(f"r{BASE_REG} is reserved by the oracle harness")

    body = [
        "  .section .opd,\"aw\"",
        "  .align 3",
        "  .globl _start",
        "_start:",
        "  .quad .Lcode, 0, 0",
        "  .text",
        ".Lcode:",
        f"  lis   {BASE_REG}, buf@ha",
        f"  addi  {BASE_REG}, {BASE_REG}, buf@l",
        f"  lwz   0, {OFF_CR_IN}({BASE_REG})",
        "  mtcr  0",
        f"  lwz   0, {OFF_XER_IN}({BASE_REG})",
        "  mtxer 0",
    ]
    for n in sorted(fprs):
        body.append(f"  lfd   {n}, {OFF_FPR_IN + n * 8}({BASE_REG})")
    for n in sorted(gprs):
        body.append(f"  ld    {n}, {OFF_GPR_IN + n * 8}({BASE_REG})")

    for line in [insn] if isinstance(insn, str) else insn:
        body.append(f"  {line}")

    # The sequence under test may legitimately overwrite the base register
    # (lmw always writes through r31), so re-materialise it from its constant
    # address before dumping state. r31 is therefore not observable.
    body.append(f"  lis   {BASE_REG}, buf@ha")
    body.append(f"  addi  {BASE_REG}, {BASE_REG}, buf@l")

    for n in range(32):
        if n == BASE_REG:
            continue
        body.append(f"  std   {n}, {OFF_GPR_OUT + n * 8}({BASE_REG})")
    for n in range(32):
        body.append(f"  stfd  {n}, {OFF_FPR_OUT + n * 8}({BASE_REG})")
    body += [
        "  mfcr  0",
        f"  stw   0, {OFF_CR_OUT}({BASE_REG})",
        "  mfxer 0",
        f"  stw   0, {OFF_XER_OUT}({BASE_REG})",
        "  li    0, 4",                      # __NR_write
        f"  mr    4, {BASE_REG}",
        "  li    3, 1",
        f"  li    5, {DUMP_SIZE}",
        "  sc",
        "  li    0, 1",                      # __NR_exit
        "  li    3, 0",
        "  sc",
    ]

    image = bytearray(DUMP_SIZE)
    for n, v in gprs.items():
        struct.pack_into(">Q", image, OFF_GPR_IN + n * 8, v & 0xFFFFFFFFFFFFFFFF)
    for n, v in fprs.items():
        struct.pack_into(">Q", image, OFF_FPR_IN + n * 8, v & 0xFFFFFFFFFFFFFFFF)
    struct.pack_into(">I", image, OFF_CR_IN, cr & 0xFFFFFFFF)
    struct.pack_into(">I", image, OFF_XER_IN, xer & 0xFFFFFFFF)
    if scratch:
        if len(scratch) > SCRATCH_SIZE:
            raise ValueError("scratch block too large")
        image[OFF_SCRATCH : OFF_SCRATCH + len(scratch)] = scratch

    body += ["  .data", "  .align 4", "buf:"]
    for i in range(0, DUMP_SIZE, 8):
        body.append("  .quad 0x" + image[i : i + 8].hex().upper())

    src = "\n".join(body) + "\n"

    with tempfile.TemporaryDirectory() as td:
        s, o, elf = (os.path.join(td, f) for f in ("t.s", "t.o", "t.elf"))
        with open(s, "w") as fh:
            fh.write(src)
        subprocess.run([AS, "-a64", "-mregnames", "-mpower7", "-maltivec", "-o", o, s], check=True)
        subprocess.run(
            [LD, "-melf64ppc", "-Ttext=0x10000000", f"-Tdata=0x{BUF_ADDR:X}", "-o", elf, o],
            check=True, capture_output=True,
        )
        # POWER7 covers every user-mode opcode used here; the 970 lacks fre.
        env = dict(os.environ, QEMU_CPU=os.environ.get("ORACLE_CPU", "power7"))
        raw = subprocess.run([QEMU, elf], check=True, capture_output=True, env=env).stdout

    if len(raw) != DUMP_SIZE:
        raise RuntimeError(f"oracle produced {len(raw)} bytes for '{insn}'")

    return {
        "r": {n: struct.unpack_from(">Q", raw, OFF_GPR_OUT + n * 8)[0] for n in range(32)
              if n != BASE_REG},
        "f": {n: struct.unpack_from(">Q", raw, OFF_FPR_OUT + n * 8)[0] for n in range(32)},
        "cr": struct.unpack_from(">I", raw, OFF_CR_OUT)[0],
        "xer": struct.unpack_from(">I", raw, OFF_XER_OUT)[0],
        "scratch": bytes(raw[OFF_SCRATCH : OFF_SCRATCH + SCRATCH_SIZE]),
    }


def fmt_hex(v):
    return f"0x{v:X}"


def fmt_double(bits):
    """Render a double the way the checked-in tests do."""
    (d,) = struct.unpack(">d", struct.pack(">Q", bits))
    return f"{d:.17f}"


def self_test():
    # cntlzw of 0xFFFFFFFF00000001 is 31 - matches the checked-in instr_cntlzw.s.
    res = run_case("cntlzw 6, 5", gprs={5: 0xFFFFFFFF00000001})
    assert res["r"][6] == 31, res["r"][6]
    # fabs, to prove the FPR path.
    res = run_case("fabs 2, 1", fprs={1: struct.unpack(">Q", struct.pack(">d", -1234.0))[0]})
    assert fmt_double(res["f"][2]) == f"{1234.0:.17f}", fmt_double(res["f"][2])
    # scratch memory round-trip through a store.
    res = run_case("stw 5, 0(6)", gprs={5: 0xAABBCCDD, 6: SCRATCH_ADDR})
    assert res["scratch"][:4] == bytes.fromhex("AABBCCDD"), res["scratch"][:8].hex()
    print("64-bit oracle self-test OK (cntlzw, fabs, stw/scratch)")


if __name__ == "__main__":
    self_test()
