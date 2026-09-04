#!/usr/bin/env python3
"""Generate tests for the newly added scalar instructions.

Every expected value comes from executing the same instruction sequence under
qemu-ppc64 (see gen_ppc_tests.py), so the tests encode observed PowerPC
behaviour rather than hand-derived results.
"""

import os
import struct

from gen_ppc_tests import ASM_DIR, SCRATCH_ADDR, run_case, fmt_double


def dbits(x):
    return struct.unpack(">Q", struct.pack(">d", x))[0]


def _mem_lines(prefix, addr, data):
    words = [data[i : i + 4].hex().upper() for i in range(0, len(data), 4)]
    return f"  #_ {prefix} {addr:08X} " + " ".join(words)


def make_case(name, insns, gprs=None, fprs=None, out_gprs=(), out_fprs=(),
              out_fprs_hex=(), scratch=None, scratch_len=0):
    """Run one sequence through the oracle and render it as a test block."""
    res = run_case(insns if isinstance(insns, str) else insns, gprs=gprs, fprs=fprs,
                   scratch=scratch)

    lines = [f"{name}:"]
    if scratch:
        lines.append(_mem_lines("MEMORY_IN", SCRATCH_ADDR, scratch))
    for n in sorted(gprs or {}):
        lines.append(f"  #_ REGISTER_IN r{n} 0x{gprs[n]:X}")
    for n in sorted(fprs or {}):
        (val,) = struct.unpack(">d", struct.pack(">Q", fprs[n]))
        lines.append(f"  #_ REGISTER_IN f{n} {val!r}")

    for insn in [insns] if isinstance(insns, str) else insns:
        lines.append(f"  {insn}")
    lines.append("  blr")

    for n in out_gprs:
        lines.append(f"  #_ REGISTER_OUT r{n} 0x{res['r'][n]:X}")
    for n in out_fprs:
        lines.append(f"  #_ REGISTER_OUT f{n} {fmt_double(res['f'][n])}")
    # lfiwax leaves an integer in the FPR, so its expectation is raw bits.
    for n in out_fprs_hex:
        lines.append(f"  #_ REGISTER_OUT f{n} 0x{res['f'][n]:X}")
    if scratch_len:
        lines.append(_mem_lines("MEMORY_OUT", SCRATCH_ADDR, res["scratch"][:scratch_len]))

    return "\n".join(lines)


def write(mnemonic, blocks):
    path = os.path.join(ASM_DIR, f"instr_{mnemonic}.s")
    with open(path, "w") as fh:
        fh.write("\n\n".join(blocks) + "\n")
    print(f"wrote {path} ({len(blocks)} cases)")


A = 0x1122334455667788
B = 0x99AABBCCDDEEFF00


def gen_isel():
    blocks = []
    # cr0 bit 2 (eq) set, then clear: isel must pick rA then rB.
    for idx, (r7, label) in enumerate(((0, "eq_set"), (1, "eq_clear")), 1):
        blocks.append(make_case(
            f"test_isel_{idx}_{label}",
            ["cmpwi cr0, r7, 0", "isel r8, r5, r6, 2"],
            gprs={5: A, 6: B, 7: r7},
            out_gprs=[5, 6, 8],
        ))
    # rA == 0 selects the literal zero, not the contents of r0.
    blocks.append(make_case(
        "test_isel_3_ra_zero",
        ["cmpwi cr0, r7, 0", "isel r8, r0, r6, 2"],
        gprs={0: A, 6: B, 7: 0},
        out_gprs=[6, 8],
    ))
    # A condition bit outside cr0: cr1 gt is bit 5.
    blocks.append(make_case(
        "test_isel_4_cr1",
        ["cmpwi cr1, r7, 0", "isel r8, r5, r6, 5"],
        gprs={5: A, 6: B, 7: 3},
        out_gprs=[5, 6, 8],
    ))
    write("isel", blocks)


def gen_popcntb():
    blocks = []
    for idx, val in enumerate((A, 0xFFFFFFFFFFFFFFFF, 0, 0x0102040810204080), 1):
        blocks.append(make_case(
            f"test_popcntb_{idx}", "popcntb r6, r5",
            gprs={5: val}, out_gprs=[5, 6],
        ))
    write("popcntb", blocks)


def gen_fcpsgn():
    blocks = []
    pairs = ((-1.0, 2.5), (1.0, -2.5), (-0.0, 4.0), (3.0, -0.0))
    for idx, (a, b) in enumerate(pairs, 1):
        blocks.append(make_case(
            f"test_fcpsgn_{idx}", "fcpsgn f3, f1, f2",
            fprs={1: dbits(a), 2: dbits(b)}, out_fprs=[1, 2, 3],
        ))
    write("fcpsgn", blocks)


ROUND_INPUTS = (2.5, -2.5, 2.4, -2.4, 0.5, -0.5, 3.0)


def gen_round(mnemonic):
    blocks = []
    for idx, val in enumerate(ROUND_INPUTS, 1):
        blocks.append(make_case(
            f"test_{mnemonic}_{idx}", f"{mnemonic} f2, f1",
            fprs={1: dbits(val)}, out_fprs=[1, 2],
        ))
    write(mnemonic, blocks)


def gen_estimates():
    # Only inputs whose estimate is exactly representable are used, so the
    # tests hold for an implementation that computes the exact value.
    blocks = []
    for idx, val in enumerate((1.0, 4.0, 0.25, 2.0, -8.0), 1):
        blocks.append(make_case(
            f"test_fre_{idx}", "fre f2, f1", fprs={1: dbits(val)}, out_fprs=[1, 2],
        ))
    write("fre", blocks)

    blocks = []
    for idx, val in enumerate((1.0, 4.0, 0.25, 64.0), 1):
        blocks.append(make_case(
            f"test_frsqrtes_{idx}", "frsqrtes f2, f1", fprs={1: dbits(val)}, out_fprs=[1, 2],
        ))
    write("frsqrtes", blocks)


MEM_PATTERN = bytes.fromhex("00112233445566778899AABBCCDDEEFF"
                            "0123456789ABCDEFFEDCBA9876543210")


def gen_lfiwax():
    blocks = []
    for idx, off in enumerate((0, 4, 20), 1):
        blocks.append(make_case(
            f"test_lfiwax_{idx}", f"lfiwax f2, 0, r5",
            gprs={5: SCRATCH_ADDR + off}, out_gprs=[5], out_fprs_hex=[2],
            scratch=MEM_PATTERN,
        ))
    write("lfiwax", blocks)


def gen_lmw():
    blocks = [make_case(
        "test_lmw_1", "lmw r28, 0(r5)",
        gprs={5: SCRATCH_ADDR}, out_gprs=[5, 28, 29, 30],
        scratch=MEM_PATTERN,
    )]
    write("lmw", blocks)


def gen_lswi():
    blocks = []
    for idx, nb in enumerate((4, 7, 12), 1):
        # Registers fill from rD upward; the trailing partial one is zeroed.
        blocks.append(make_case(
            f"test_lswi_{idx}_nb{nb}", f"lswi r6, r5, {nb}",
            gprs={5: SCRATCH_ADDR}, out_gprs=[5, 6, 7, 8],
            scratch=MEM_PATTERN,
        ))
    write("lswi", blocks)


def gen_stswi():
    blocks = []
    for idx, nb in enumerate((4, 7, 12), 1):
        blocks.append(make_case(
            f"test_stswi_{idx}_nb{nb}", f"stswi r6, r5, {nb}",
            gprs={5: SCRATCH_ADDR, 6: A, 7: B, 8: 0xCAFEBABEDEADBEEF},
            out_gprs=[5, 6, 7, 8],
            scratch=bytes(16), scratch_len=16,
        ))
    write("stswi", blocks)


# isel, fcpsgn and lfiwax are implemented but unreachable: ReXGlue's
# disassembler runs a Xenon dialect mask that excludes PPCISEL and POWER6, so
# those encodings decode as raw data. Their generators are kept for the day the
# mask changes, but they are not part of the default run.
if __name__ == "__main__":
    gen_popcntb()
    for m in ("frin", "friz", "frip", "frim"):
        gen_round(m)
    gen_estimates()
    gen_lmw()
    gen_lswi()
    gen_stswi()
