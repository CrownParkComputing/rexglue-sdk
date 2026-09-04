#!/usr/bin/env python3
"""Test cases for the newly added integer VMX instructions.

Expected values are produced by the qemu-ppc oracle in gen_vmx_tests.py, so
they are observed PowerPC results rather than hand-computed ones.

Each instruction gets two cases built from two differently-shaped input sets.
Patterns are chosen to exercise sign, overflow and saturation edges rather than
just small positive values.
"""

from gen_vmx_tests import emit

# name -> (primary pattern, alternate pattern)
PATTERNS = {
    "A": ([0x0102F17F, 0x80FF0A0B, 0x7F7F8081, 0x0000FFFF],
          [0xFFFFFFFF, 0x00000000, 0x5A5AA5A5, 0x12345678]),
    "B": ([0x03047E02, 0x017F80FE, 0x0102FF01, 0xFFFF0001],
          [0x00000002, 0xFFFFFFFF, 0xA5A55A5A, 0x9ABCDEF0]),
    "C": ([0x00000001, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF],
          [0x7FFFFFFE, 0x00000005, 0x00000000, 0x80000001]),
    "D": ([0x00010002, 0x80007FFF, 0xFFFF0001, 0x7FFF8000],
          [0x7FFF7FFF, 0x80008000, 0x00010001, 0xFFFFFFFF]),
    # Saturation-heavy inputs for the sum-across instructions.
    "S": ([0x7FFFFFFF, 0x7FFFFFFF, 0x00000010, 0x80000000],
          [0x00000002, 0xFFFFFFFF, 0xA5A55A5A, 0x9ABCDEF0]),
    "T": ([0x7FFFFFFF, 0x00000002, 0xFFFFFFFF, 0x80000000],
          [0xFFFFFFFF, 0x00000000, 0x5A5AA5A5, 0x12345678]),
}

# mnemonic -> input patterns, in operand order after vD (which is always v5).
CASES = {
    "vupkhpx": "A",
    "vupklpx": "A",
    "vmuleub": "AB",
    "vmulesb": "AB",
    "vmuleuh": "AB",
    "vmulesh": "AB",
    "vmuloub": "AB",
    "vmulosb": "AB",
    "vmulouh": "AB",
    "vmulosh": "AB",
    "vmaxuw": "AB",
    "vavguw": "AB",
    "vaddcuw": "AB",
    "vsubcuw": "AB",
    "vrlb": "AB",
    "vpkpx": "AB",
    "vsum4ubs": "AC",
    "vsum4sbs": "AC",
    "vsum4shs": "DC",
    "vsum2sws": "ST",
    "vsumsws": "ST",
    "vmsumubm": "ABC",
    "vmsummbm": "ABC",
    "vmsumuhm": "ABC",
    "vmsumuhs": "ABC",
    "vmsumshm": "DDC",
    "vmsumshs": "DDC",
    "vmhaddshs": "DDD",
    "vmhraddshs": "DDD",
    "vmladduhm": "DDD",
}

REG_NAMES = ["v3", "v4", "v6"]


def build_cases(mnemonic, pattern_names):
    cases = []
    for variant in (0, 1):
        regs = {}
        names = REG_NAMES[: len(pattern_names)]
        for reg, key in zip(names, pattern_names):
            regs[reg] = PATTERNS[key][variant]
        operands = ", ".join(["v5"] + names)
        cases.append((f"{mnemonic} {operands}", regs, names + ["v5"]))
    return cases


if __name__ == "__main__":
    for mnemonic, pattern_names in CASES.items():
        cases = build_cases(mnemonic, pattern_names)
        if mnemonic == "vpkpx":
            # Keep the vector pair from the repo's previously disabled vpkpx
            # test, so its expectation is preserved rather than dropped.
            cases.append((
                "vpkpx v5, v3, v4",
                {"v3": [0x00101820, 0x01283038, 0x00404850, 0x01586068],
                 "v4": [0x01707880, 0x00889098, 0x01A0A8B0, 0x00B8C0C8]},
                ["v3", "v4", "v5"],
            ))
        emit(mnemonic, cases)
