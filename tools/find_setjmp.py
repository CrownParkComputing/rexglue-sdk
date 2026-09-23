#!/usr/bin/env python3
"""Find a title's setjmp/longjmp pair in its generated code.

  find_setjmp.py <port-dir>

Codegen finds the save/restore helpers and the switch tables on its own but not
these, and undeclared, the longjmp is emitted as an ORDINARY CALL: control never
unwinds and the caller continues with non-volatile registers restored from a
stale jmp_buf. The damage surfaces far away - an unrelated function calling a
pointer it never computed, or computing a wrong value from a register it never
set. Both Geometry Wars titles rendered nothing until these were declared.

The signature is unmistakable once you know it. f14-f31 is eighteen registers,
and setjmp/longjmp are the only functions that move all eighteen at a stride of
8 from a POINTER ARGUMENT starting at offset 0. The __savefpr/__restfpr helpers
touch the same registers but address them from r12 at negative offsets, which is
what separates the two.
"""
import pathlib
import re
import sys

FP = re.compile(r"//\s+(lfd|stfd)\s+f(\d+),(-?\d+)\((r\d+)\)")
FUNC = re.compile(r"DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")


def scan(port):
    found = {"setjmp": [], "longjmp": []}
    gen = pathlib.Path(port) / "generated" / "default"
    for path in sorted(gen.glob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        # Split into function bodies so a match is attributed correctly.
        marks = [(m.start(), m.group(1)) for m in FUNC.finditer(text)]
        for i, (start, addr) in enumerate(marks):
            end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
            body = text[start:end]
            by_base = {}
            for kind, reg, off, base in FP.findall(body):
                reg, off = int(reg), int(off)
                if reg < 14:            # only the non-volatile set counts
                    continue
                by_base.setdefault((kind, base), set()).add((reg, off))
            for (kind, base), entries in by_base.items():
                regs = {r for r, _ in entries}
                offs = sorted(o for _, o in entries)
                if len(regs) < 18 or min(offs) != 0:
                    continue
                # Stride 8, ascending: a jmp_buf laid out f14..f31 back to back.
                if any(b - a != 8 for a, b in zip(offs, offs[1:])):
                    continue
                found["setjmp" if kind == "stfd" else "longjmp"].append(addr)
    return found


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    port = pathlib.Path(sys.argv[1])
    res = scan(port)
    slug = port.name.replace("-recomp", "")
    if not res["setjmp"] and not res["longjmp"]:
        print(f"{slug}: no setjmp/longjmp pair found")
        return 1
    print(f"# {slug}")
    for name in ("longjmp", "setjmp"):
        for addr in res[name]:
            print(f"{name}_address = 0x{addr}")
    if len(res["setjmp"]) != 1 or len(res["longjmp"]) != 1:
        print(f"# NOTE: expected one of each, found {len(res['longjmp'])} longjmp / "
              f"{len(res['setjmp'])} setjmp - check before using")
    return 0


if __name__ == "__main__":
    sys.exit(main())
