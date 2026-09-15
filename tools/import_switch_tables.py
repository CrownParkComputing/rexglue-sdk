#!/usr/bin/env python3
"""Convert XenonRecomp [[switch]] jump tables into a rexglue codegen include.

  import_switch_tables.py <xenonrecomp_jumptable.toml> <out.toml>

XenonRecomp keys a table at `base` - the instruction that starts computing the
table address - and rexglue keys `address` the same way: it picks the table up
at that instruction and holds it until a `bctr` consumes it. So the two are the
same field under different names, and the conversion is a rename plus dropping
`default`, which rexglue derives from the fallthrough instead of being told.

Tables recovered by hand are worth keeping: a dispatch rexglue cannot see the
bounds of becomes an indirect call through whatever the index register happens
to hold, which shows up at runtime as a call to a nonsense address and a title
that renders nothing.
"""
import re
import sys


def parse(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    tables, cur = [], None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if line == "[[switch]]":
            cur = {"labels": []}
            tables.append(cur)
            continue
        if cur is None or not line:
            continue
        if m := re.match(r"(base|r)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", line):
            cur[m.group(1)] = int(m.group(2), 0)
        elif line.startswith("labels"):
            cur["_in_labels"] = True
            line = line.split("=", 1)[1]
        if cur.get("_in_labels"):
            cur["labels"] += [int(v, 0) for v in re.findall(r"0x[0-9A-Fa-f]+", line)]
            if "]" in line:
                cur["_in_labels"] = False
    return [t for t in tables if "base" in t and "r" in t and t["labels"]]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    tables = parse(src)

    # Duplicates are common in these files: the same dispatch recovered by two
    # tools that key it differently. The first wins in rexglue too, so emitting
    # both is harmless, but dropping them keeps the include readable.
    seen, out = set(), []
    for t in tables:
        if t["base"] in seen:
            continue
        seen.add(t["base"])
        out.append(t)

    with open(dst, "w", encoding="utf-8") as f:
        f.write(f"# Switch tables converted from {src}\n")
        f.write("# by tools/import_switch_tables.py - do not edit by hand.\n\n")
        for t in out:
            f.write("[[switch_tables]]\n")
            f.write(f"address = 0x{t['base']:08X}\n")
            f.write(f"register = {t['r']}\n")
            f.write("labels = [\n")
            for lab in t["labels"]:
                f.write(f"    0x{lab:08X},\n")
            f.write("]\n\n")
    print(f"{len(out)} tables -> {dst}"
          f"{f' ({len(tables) - len(out)} duplicates dropped)' if len(tables) != len(out) else ''}")


if __name__ == "__main__":
    main()
