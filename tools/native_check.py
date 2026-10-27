#!/usr/bin/env python3
"""Gate: is a module of a port free of emulated (recompiled) guest code?

  native_check.py <port> [--module NAME] [--binary PATH] [--reach CSV]
                         [--symbols out/native/symbols.json] [--config linux]
                         [--require-reach] [--list N] [--json PATH]

Reads <port>/native.toml (shape frozen in docs/native/FORMATS.md):

  [module.d3d9]
  ranges  = ["82690000-8269FFFF"]     # or: range = "0x82690000-0x8269FFFF"
  policy  = "no-guest-code"           # no-guest-code | banned | reach-zero
  banned  = ["8269ADD8", ...]         # policy=banned: the addresses that matter
  allow   = ["82691000"]              # exceptions, never offenders
  band    = "docs/d3d/band.json"      # optional: ranges taken from it when present

The rule (see FORMATS.md, "What counts as guest code"):

  * Static, from the linked image via out/native/symbols.json (native_map.py):
    "guest code reachable" == a GENERATED function inside the range, i.e. the
    weak alias sub_X still resolves to codegen's __imp__sub_X body. A DISPLACED
    function (strong sub_X from the port) passes the static check even though
    its __imp__sub_X body may survive the link (a forwarding hook keeps it;
    only the reach trace can tell whether it still runs). DROPPED functions
    pass (no code in the image).
  * Dynamic, from a REX_TRACE_REACH CSV (or reach_from_perf.sh output):
    "guest code reached" == any address inside the range with count > 0. The
    counter lives in the generated prologue of __imp__sub_X, so a displaced
    function whose hook forwards to the original still counts. This is the
    stricter of the two checks and the one the M4 gate is judged on.

Policies:
  no-guest-code  static offenders: generated in range; dynamic: reached in range
  banned         same as no-guest-code, restricted to the `banned` list
  reach-zero     static check skipped (guest code may exist); dynamic: reached in range

Exit status: 0 pass, 1 offenders (listed with counts), 2 configuration error.
Without --reach the dynamic check is skipped with a note (exit 0 if static
passes), unless --require-reach.
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

try:
    import tomllib
except ModuleNotFoundError:  # python < 3.11
    tomllib = None

POLICIES = ("no-guest-code", "banned", "reach-zero")
ADDR_RE = re.compile(r"^(?:0x)?([0-9A-Fa-f]{8})$")
RANGE_RE = re.compile(r"^(?:0x)?([0-9A-Fa-f]{8})\s*[-:]\s*(?:0x)?([0-9A-Fa-f]{8})$")
SUB_RE = re.compile(r"^([0-9a-fA-F]+)\s+([A-Za-z])\s+(__imp__)?sub_([0-9A-Fa-f]{8})$")


# --------------------------------------------------------------------------- #
# loaders (imported by native_report.py and native_targets.py)
# --------------------------------------------------------------------------- #

def norm_addr(value):
    """'0x823ddA50' | '823DDA50' | 2183977552 -> '823DDA50'; None if not an address."""
    if isinstance(value, int):
        return f"{value:08X}"
    m = ADDR_RE.match(str(value).strip())
    return m.group(1).upper() if m else None


def parse_range(text):
    m = RANGE_RE.match(str(text).strip())
    if not m:
        raise ValueError(f"bad range {text!r} (want 82690000-8269FFFF)")
    lo, hi = int(m.group(1), 16), int(m.group(2), 16)
    if lo > hi:
        raise ValueError(f"bad range {text!r}: start after end")
    return lo, hi


def in_ranges(addr, ranges):
    a = int(addr, 16)
    return any(lo <= a <= hi for lo, hi in ranges)


def load_toml(path):
    if tomllib is None:
        sys.exit("native_check.py needs python >= 3.11 (tomllib)")
    with open(path, "rb") as fh:
        return tomllib.load(fh)


def load_band_json(path):
    """Ranges from A1's band.json. Accepts {"ranges": [...]}, {"range": ".."},
    {"start": .., "end": ..} or a list of such objects."""
    with open(path) as fh:
        data = json.load(fh)
    items = data if isinstance(data, list) else [data]
    ranges = []
    for item in items:
        if not isinstance(item, dict):
            continue
        if "ranges" in item:
            ranges += [parse_range(r) for r in item["ranges"]]
        elif "range" in item:
            ranges.append(parse_range(item["range"]))
        elif "start" in item and "end" in item:
            ranges.append((int(norm_addr(item["start"]), 16), int(norm_addr(item["end"]), 16)))
    return ranges


def load_native_toml(port):
    """{name: {"ranges": [(lo,hi)], "policy": str, "banned": set, "allow": set,
               "entrypoints": path|None, "source": str}}.
    Returns {} when the port has no native.toml."""
    port = pathlib.Path(port)
    path = port / "native.toml"
    if not path.is_file():
        return {}
    doc = load_toml(path)
    modules = {}
    for name, cfg in (doc.get("module") or {}).items():
        if not isinstance(cfg, dict):
            raise ValueError(f"[module.{name}] must be a table")
        ranges, source = [], "native.toml"
        band = cfg.get("band")
        if band and (port / band).is_file():
            ranges, source = load_band_json(port / band), band
        if not ranges:
            if "ranges" in cfg:
                ranges = [parse_range(r) for r in cfg["ranges"]]
            elif "range" in cfg:
                ranges = [parse_range(cfg["range"])]
        policy = cfg.get("policy", "no-guest-code")
        if policy not in POLICIES:
            raise ValueError(f"[module.{name}] policy {policy!r} not one of {POLICIES}")
        banned = {norm_addr(a) for a in cfg.get("banned", [])} - {None}
        allow = {norm_addr(a) for a in cfg.get("allow", [])} - {None}
        if not ranges and not banned:
            raise ValueError(f"[module.{name}] needs ranges/range/band or banned")
        modules[name] = {
            "ranges": ranges, "policy": policy, "banned": banned, "allow": allow,
            "entrypoints": cfg.get("entrypoints"), "source": source,
            "description": cfg.get("description", ""),
        }
    return modules


def load_symbols(path):
    """symbols.json -> {ADDR: entry}. Raises FileNotFoundError."""
    with open(path) as fh:
        doc = json.load(fh)
    if not str(doc.get("format", "")).startswith("rex-native-symbols/"):
        raise ValueError(f"{path}: not a rex-native-symbols file")
    return doc


def symbols_from_binary(binary, generated):
    """Fallback when symbols.json is missing: classify from nm alone.
    `generated` is the set of addresses codegen emitted (or None: every
    sub_X seen in the image)."""
    out = subprocess.run(["nm", "--defined-only", str(binary)], capture_output=True,
                         text=True, check=True).stdout
    weak, strong, imp = set(), set(), set()
    for line in out.splitlines():
        m = SUB_RE.match(line.strip())
        if not m:
            continue
        addr = m.group(4).upper()
        if m.group(3):
            imp.add(addr)
        elif m.group(2) in "Ww":
            weak.add(addr)
        elif m.group(2) in "Tt":
            strong.add(addr)
    addrs = generated if generated is not None else (weak | strong | imp)
    symbols = {}
    for a in sorted(addrs):
        if a in strong:
            st = "displaced"
        elif a in weak or a in imp:
            st = "generated"
        else:
            st = "dropped"
        symbols[a] = {"name": f"sub_{a}", "status": st, "defined_in": None,
                      "partition": None, **({"imp_present": a in imp} if st == "displaced" else {})}
    return {"format": "rex-native-symbols/1(nm-fallback)", "binary": str(binary),
            "counts": {s: sum(1 for e in symbols.values() if e["status"] == s)
                       for s in ("generated", "displaced", "dropped")},
            "symbols": symbols}


def load_reach(path):
    """REX_TRACE_REACH CSV (or reach_from_perf.sh output) -> {ADDR: count}.
    Rows whose addr is not 8 hex digits are skipped (hand-written prologues)."""
    counts = {}
    with open(path, errors="ignore") as fh:
        for i, line in enumerate(fh):
            s = line.strip()
            if not s or (i == 0 and s.lower().startswith("addr")):
                continue
            parts = s.split(",")
            if len(parts) < 2:
                continue
            addr = norm_addr(parts[0])
            if addr is None:
                continue
            try:
                counts[addr] = counts.get(addr, 0) + int(parts[1])
            except ValueError:
                continue
    return counts


def find_binary(port, config):
    build = pathlib.Path(port) / "out" / "build" / config
    best = None
    for entry in build.glob("*"):
        if entry.is_file() and os.access(entry, os.X_OK) and entry.suffix not in (".so", ".dll", ".cmake", ".ninja", ".toml"):
            size = entry.stat().st_size
            if best is None or size > best[0]:
                best = (size, entry)
    return best[1] if best else None


# --------------------------------------------------------------------------- #
# the check
# --------------------------------------------------------------------------- #

def module_members(module, symbols):
    """Addresses this module judges: everything in range (plus the banned list)."""
    if module["policy"] == "banned":
        return sorted(module["banned"])
    return sorted(a for a in symbols if in_ranges(a, module["ranges"]))


def check_module(name, module, symdoc, reach):
    symbols = symdoc["symbols"]
    members = module_members(module, symbols)
    allow = module["allow"]
    result = {"module": name, "policy": module["policy"], "members": len(members),
              "static": None, "dynamic": None, "static_offenders": [], "dynamic_offenders": []}

    if module["policy"] != "reach-zero":
        off = []
        for a in members:
            if a in allow:
                continue
            e = symbols.get(a)
            if e is None:
                # banned address codegen never emitted: nothing to displace, passes
                continue
            if e["status"] == "generated":
                off.append({"addr": a, "count": (reach or {}).get(a, 0)})
        result["static"] = "pass" if not off else "fail"
        result["static_offenders"] = off

    if reach is not None:
        off = []
        judged = set(members)
        if module["policy"] != "banned":
            # reach rows for addresses outside symbols.json but inside the range
            judged |= {a for a in reach if in_ranges(a, module["ranges"])}
        for a in sorted(judged):
            if a in allow:
                continue
            c = reach.get(a, 0)
            if c > 0:
                st = symbols.get(a, {}).get("status", "unknown")
                off.append({"addr": a, "count": c, "status": st})
        result["dynamic"] = "pass" if not off else "fail"
        result["dynamic_offenders"] = off
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--module", help="check one module (default: all in native.toml)")
    ap.add_argument("--binary", help="linked title executable (used when symbols.json is missing)")
    ap.add_argument("--symbols", help="symbols.json (default <port>/out/native/symbols.json)")
    ap.add_argument("--config", default="linux")
    ap.add_argument("--reach", help="REX_TRACE_REACH CSV (or reach_from_perf.sh output) for the dynamic check")
    ap.add_argument("--require-reach", action="store_true", help="fail when no reach CSV is given")
    ap.add_argument("--list", type=int, default=25, help="offenders to print per module (default 25, 0 = all)")
    ap.add_argument("--json", help="write the result as json")
    args = ap.parse_args()

    port = pathlib.Path(args.port).resolve()
    try:
        modules = load_native_toml(port)
    except (ValueError, OSError) as exc:
        print(f"native_check: {exc}", file=sys.stderr)
        return 2
    if not modules:
        print(f"native_check: {port}/native.toml has no [module.*] tables (or is missing); nothing to check")
        return 0
    if args.module:
        if args.module not in modules:
            print(f"native_check: no [module.{args.module}] in native.toml (have: {', '.join(modules)})", file=sys.stderr)
            return 2
        modules = {args.module: modules[args.module]}

    sym_path = pathlib.Path(args.symbols) if args.symbols else port / "out" / "native" / "symbols.json"
    symdoc = None
    if sym_path.is_file():
        symdoc = load_symbols(sym_path)
        if args.binary and os.path.abspath(args.binary) != symdoc.get("binary"):
            print(f"native_check: note: {sym_path} was mapped from {symdoc.get('binary')}, not --binary; "
                  f"re-run native_map.py for an exact answer")
    else:
        binary = pathlib.Path(args.binary) if args.binary else find_binary(port, args.config)
        if not binary or not binary.is_file():
            print(f"native_check: no {sym_path} and no binary; run tools/native_map.py {port}", file=sys.stderr)
            return 2
        print(f"native_check: no {sym_path}; classifying from nm {binary} (every sub_X in the image)")
        symdoc = symbols_from_binary(binary, None)

    reach = None
    if args.reach:
        if not pathlib.Path(args.reach).is_file():
            print(f"native_check: reach CSV {args.reach} not found", file=sys.stderr)
            return 2
        reach = load_reach(args.reach)
    elif args.require_reach:
        print("native_check: --require-reach given but no --reach CSV", file=sys.stderr)
        return 2

    results, failed = [], False
    print(f"native_check: {port.name}  symbols={sym_path if sym_path.is_file() else 'nm'}"
          f"  reach={args.reach or '(none: dynamic check skipped)'}")
    for name, module in modules.items():
        r = check_module(name, module, symdoc, reach)
        results.append(r)
        rng = ", ".join(f"{lo:08X}-{hi:08X}" for lo, hi in module["ranges"]) or "(banned list only)"
        print(f"\n[module.{name}]  policy={module['policy']}  ranges={rng}  members={r['members']}")
        for kind in ("static", "dynamic"):
            verdict = r[kind]
            if verdict is None:
                why = "not applicable to reach-zero" if kind == "static" else "no reach CSV"
                print(f"  {kind:<8} skipped ({why})")
                continue
            offenders = r[f"{kind}_offenders"]
            print(f"  {kind:<8} {verdict.upper()}  {len(offenders)} offender(s)")
            if verdict == "fail":
                failed = True
                shown = offenders if args.list == 0 else sorted(offenders, key=lambda o: -o["count"])[:args.list]
                for o in shown:
                    extra = f"  [{o['status']}]" if "status" in o else ""
                    print(f"    sub_{o['addr']}  count={o['count']}{extra}")
                if len(offenders) > len(shown):
                    print(f"    ... {len(offenders) - len(shown)} more (--list 0 for all)")

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"format": "rex-native-check/1", "port": str(port), "reach": args.reach,
                       "results": results, "pass": not failed}, fh, indent=1)
            fh.write("\n")
    print(f"\nnative_check: {'FAIL' if failed else 'PASS'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
