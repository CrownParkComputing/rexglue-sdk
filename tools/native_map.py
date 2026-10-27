#!/usr/bin/env python3
"""Map every generated guest function to what the linked binary actually holds.

  native_map.py <port-dir> [--binary path] [--out path] [--config linux]

Codegen emits each guest function twice: `__imp__sub_X` is the recompiled
body and `sub_X` a weak alias to it. A port displaces a function by defining
a strong `sub_X` in its own sources, and the linker's --gc-sections drops a
function nothing references. The linked image is therefore the only honest
answer to "which guest functions are still emulated"; this reads it with nm
and writes out/native/symbols.json (format frozen in docs/native/FORMATS.md):

  generated  weak sub_X in the image, i.e. the recompiled body is what runs
  displaced  strong sub_X in the image, defined by a port object file
  dropped    no sub_X in the image at all (never referenced, gc'd)

The generated set comes from generated/*/codegen.partition.json (every
address codegen assigned to a partition), falling back to a scan of the
recompiled sources for DEFINE_REX_FUNC when no partition file exists.
"""
import argparse
import glob
import json
import os
import pathlib
import re
import subprocess
import sys

SUB_RE = re.compile(r"^([0-9a-fA-F]+)\s+([A-Za-z])\s+(__imp__)?(sub_[0-9A-Fa-f]{8})$")
DEFINE_RE = re.compile(r"DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)")


def nm(path):
    """Yield (symbol_type, has_imp_prefix, name) for every defined sub_ symbol."""
    try:
        out = subprocess.run(["nm", "--defined-only", str(path)], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f"nm failed on {path}: {exc}")
    for line in out.splitlines():
        m = SUB_RE.match(line.strip())
        if m:
            yield m.group(2), bool(m.group(3)), m.group(4).upper().replace("SUB_", "sub_")


def generated_functions(port):
    """{ADDR: partition_index_or_None} for every function codegen emitted a body for.

    The recompiled sources are the truth: codegen.partition.json is a stable
    assignment map that keeps addresses codegen has since stopped emitting
    (PGR4 carries 237 such stale entries), so it only annotates here.
    """
    funcs = {}
    for src in sorted(glob.glob(os.path.join(port, "generated", "*", "*_recomp.*.cpp"))):
        with open(src, errors="ignore") as fh:
            for m in DEFINE_RE.finditer(fh.read()):
                funcs[m.group(1).upper()] = None
    if not funcs:
        sys.exit(f"no generated functions under {port}/generated (run codegen first)")
    stale = 0
    for part in sorted(glob.glob(os.path.join(port, "generated", "*", "codegen.partition.json"))):
        with open(part) as fh:
            data = json.load(fh)
        for addr, idx in data.get("assignments", {}).items():
            if addr.upper() in funcs:
                funcs[addr.upper()] = idx
            else:
                stale += 1
    return funcs, stale


def find_binary(port, config):
    """The linked title executable: the largest executable file in out/build/<config>."""
    build = os.path.join(port, "out", "build", config)
    best = None
    for entry in glob.glob(os.path.join(build, "*")):
        if os.path.isfile(entry) and os.access(entry, os.X_OK) and not entry.endswith((".so", ".dll", ".cmake", ".ninja", ".toml")):
            size = os.path.getsize(entry)
            if best is None or size > best[0]:
                best = (size, entry)
    if best is None:
        sys.exit(f"no linked executable in {build}; pass --binary")
    return best[1]


def port_objects(port, config):
    """Object files compiled from the port's own sources (not the generated OBJECT lib)."""
    build = os.path.join(port, "out", "build", config)
    objs = []
    for obj in glob.glob(os.path.join(build, "CMakeFiles", "*.dir", "**", "*.o"), recursive=True):
        rel = os.path.relpath(obj, build)
        if "_recomp.dir" in rel or "/generated/" in rel or "cmake_pch" in rel:
            continue
        objs.append(obj)
    return sorted(objs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--binary", help="linked title executable (default: largest in out/build/<config>)")
    ap.add_argument("--config", default="linux", help="out/build/<config> to read (default linux)")
    ap.add_argument("--out", help="output path (default <port>/out/native/symbols.json)")
    args = ap.parse_args()

    port = os.path.abspath(args.port)
    binary = os.path.abspath(args.binary) if args.binary else find_binary(port, args.config)
    funcs, stale = generated_functions(port)

    weak, strong, imp = set(), set(), set()
    for stype, is_imp, name in nm(binary):
        if is_imp:
            imp.add(name)
        elif stype in "Ww":
            weak.add(name)
        elif stype in "Tt":
            strong.add(name)

    # Strong sub_X definitions in the port's own object files -> defined_in.
    defined_in = {}
    for obj in port_objects(port, args.config):
        for stype, is_imp, name in nm(obj):
            if not is_imp and stype == "T":
                defined_in.setdefault(name, os.path.relpath(obj, os.path.join(port, "out", "build", args.config)))

    symbols = {}
    counts = {"generated": 0, "displaced": 0, "dropped": 0}
    for addr in sorted(funcs):
        name = f"sub_{addr}"
        if name in strong:
            status, src = "displaced", defined_in.get(name)
        elif name in weak or name in imp:
            status, src = "generated", None
        else:
            status, src = "dropped", None
        counts[status] += 1
        entry = {"name": name, "status": status, "defined_in": src, "partition": funcs[addr]}
        if status == "displaced":
            entry["imp_present"] = name in imp
        symbols[addr] = entry

    # Strong sub_X in the image that codegen never emitted: a port hooking an
    # address outside the generated set. Reported, not counted against codegen.
    extra = sorted(n for n in strong if n[4:] not in funcs)

    out_path = args.out or os.path.join(port, "out", "native", "symbols.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    doc = {
        "format": "rex-native-symbols/1",
        "port": port,
        "binary": binary,
        "counts": counts,
        "stale_partition_entries": stale,
        "extra_strong": [{"name": n, "defined_in": defined_in.get(n)} for n in extra],
        "symbols": symbols,
    }
    with open(out_path, "w") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")

    total = len(funcs)
    print(f"{os.path.basename(port)}: {total} generated functions -> "
          f"{counts['generated']} generated, {counts['displaced']} displaced, {counts['dropped']} dropped"
          f"{f', {len(extra)} strong outside codegen' if extra else ''} -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
