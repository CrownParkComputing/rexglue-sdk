#!/usr/bin/env python3
"""Check the lvlx/lvrx/stvlx/stvrx mask tables against the PowerPC definition.

qemu has no Cell or Xenon CPU model, so these instructions cannot be run under
the oracle the way the AltiVec ones can - an attempt dies with SIGILL, and
binutils only assembles them at all under -mcell. Their definition is exact
though, so the tables can be checked against it directly, which is worth doing
because the suite has no case for any of them and Hydro Thunder's audio mixer
uses lvlx/lvrx eighteen times in four functions.

Models what the generated code does:

    lvlx  vD = shuffle_epi8(load16(EA & ~0xF), VectorMaskL[(EA & 0xF) * 16])
    lvrx  vD = (EA & 0xF) ? shuffle_epi8(load16(EA & ~0xF), VectorMaskR[...]) : 0

and compares against the architected result, remembering that vector registers
are stored fully byte-reversed: PowerPC byte k is host byte 15 - k.
"""
import os, re, sys

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "..", "include", "rex", "ppc", "intrinsics.h")


def load_table(name):
    text = open(HEADER).read()
    m = re.search(rf"inline uint8_t {name}\[\] = {{(.*?)}};", text, re.S)
    if not m:
        raise SystemExit(f"{name} not found in {HEADER}")
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
    if len(vals) != 256:
        raise SystemExit(f"{name} has {len(vals)} bytes, expected 256")
    return vals


def emulate(mask, mem, sh, zero_when_aligned):
    """What the generated shuffle produces, in PowerPC byte order."""
    if zero_when_aligned and sh == 0:
        host = [0] * 16
    else:
        row = mask[sh * 16:(sh + 1) * 16]
        host = [0 if b == 0xFF else mem[b & 0x0F] for b in row]
    return [host[15 - k] for k in range(16)]   # registers are byte-reversed


def spec_lvlx(mem, sh):
    # vD byte k = MEM[EA + k] while that stays inside EA's 16-byte block.
    return [mem[sh + k] if sh + k < 16 else 0 for k in range(16)]


def spec_lvrx(mem, sh):
    # vD byte k = MEM[(EA & ~0xF) + k - (16 - sh)], right justified, rest zero.
    return [mem[k - (16 - sh)] if k >= 16 - sh else 0 for k in range(16)]


def main():
    mem = list(range(16))          # block bytes 0x00..0x0F, self-identifying
    failures = 0
    for name, table, spec, zero_aligned in (
            ("lvlx", load_table("VectorMaskL"), spec_lvlx, False),
            ("lvrx", load_table("VectorMaskR"), spec_lvrx, True)):
        for sh in range(16):
            got = emulate(table, mem, sh, zero_aligned)
            want = spec(mem, sh)
            if got != want:
                failures += 1
                print(f"FAIL {name} align {sh}")
                print(f"  emitted {' '.join(f'{b:02X}' for b in got)}")
                print(f"  spec    {' '.join(f'{b:02X}' for b in want)}")
        print(f"{name}: 16 alignments checked")
    if failures:
        print(f"\n{failures} mismatch(es)")
        return 1
    print("\nall alignments match the PowerPC definition")
    return 0


if __name__ == "__main__":
    sys.exit(main())
