#!/usr/bin/env python3
"""Score how much of a port's kernel surface is native rather than ReXGlue.

  native_report.py <port-dir> [--trace used.txt] [--binary path] [--write]
                   [--symbols out/native/symbols.json] [--reach reach.csv]
                   [--perf reach_perf.csv] [--top N]

Two numbers matter and they are different:

  * IMPORTED  - what the XEX lists. Includes plenty the title never calls.
  * CALLED    - what it actually reached at runtime, from a trace produced by
                REX_TRACE_IMPORTS=<file> ./run.sh. This is the real work list.

Coverage is measured against CALLED. Kernel exports in the SDK are weak, so a
REX_NATIVE_HOOK in the port's own sources displaces one; --binary confirms that
from the linked image (T = the port's, W = still ReXGlue's) rather than trusting
the source scan.

Guest functions (second section, written beside the kernel table):

  * symbols.json (tools/native_map.py) says which recompiled functions are
    still GENERATED (weak sub_X = codegen's body), DISPLACED (the port's
    strong sub_X) or DROPPED (gc'd). Per module from <port>/native.toml
    (module ranges, see docs/native/FORMATS.md) plus "all".
  * --reach: REX_TRACE_REACH CSV -> which were reached and how many calls
    each took, so the module is weighted by calls, not by function count.
  * --perf: the same CSV shape from tools/reach_from_perf.sh -> CPU samples,
    the weight the "100% native by CPU weight" gate is judged on.

The kernel headline line ("N of M imports ... are native (P%)") is unchanged;
tools/pipeline.py greps it.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from native_check import load_native_toml, load_symbols, load_reach, in_ranges  # noqa: E402
except ImportError:  # tools dir copied without native_check.py
    load_native_toml = load_symbols = load_reach = in_ranges = None

CATEGORIES = [
    ("GPU / video", lambda n: n.startswith("Vd")),
    ("audio", lambda n: n.startswith(("XMA", "XAudio"))),
    ("input", lambda n: n.startswith("XInput")),
    ("profile / saves", lambda n: n.startswith(("XamContent", "XamUser", "XamShow", "XamProfile"))),
    ("XAM misc", lambda n: n.startswith(("Xam", "XMsg", "XNotify"))),
    ("file I/O", lambda n: n.startswith(("NtCreateFile", "NtReadFile", "NtWriteFile", "NtQueryInformationFile",
                                        "NtQueryFullAttributes", "NtOpenFile", "NtClose", "NtQueryDirectoryFile",
                                        "NtQueryVolumeInformationFile", "NtSetInformationFile", "NtFlushBuffersFile"))),
    ("memory", lambda n: n.startswith(("NtAllocateVirtualMemory", "NtFreeVirtualMemory", "NtProtectVirtualMemory",
                                       "NtQueryVirtualMemory", "Mm"))),
    ("threads / TLS", lambda n: n.startswith(("ExCreateThread", "ExTerminateThread", "KeTls", "KeSetAffinityThread",
                                              "KeSetBasePriorityThread", "KeQueryBasePriorityThread", "NtResumeThread",
                                              "NtSuspendThread", "NtYieldExecution", "Ob", "KeGetCurrentProcessType",
                                              "KeQueryPerformanceFrequency"))),
    ("networking", lambda n: n.startswith(("NetDll", "XNet", "XOnline", "XSession"))),
]


def categorise(name):
    for label, match in CATEGORIES:
        if match(name):
            return label
    return "sync / events / rtl / misc"


def native_names(port):
    """Imports the port answers itself, from REX_NATIVE_HOOK in its sources."""
    found = set()
    src = pathlib.Path(port) / "src"
    for path in src.rglob("*.cpp") if src.is_dir() else []:
        for m in re.finditer(r"REX_NATIVE_HOOK\s*\(\s*__imp__([A-Za-z0-9_]+)\s*\)",
                             path.read_text(encoding="utf-8", errors="replace")):
            found.add(m.group(1))
    return found


def linked_strong(binary):
    """Imports whose definition in the linked image is the port's, not the SDK's."""
    strong = set()
    try:
        out = subprocess.run(["nm", "--defined-only", binary], capture_output=True,
                             text=True, check=True).stdout
    except Exception:
        return None
    for line in out.splitlines():
        m = re.match(r"\S+\s+([A-Za-z])\s+__imp__([A-Za-z][A-Za-z0-9_]*)$", line.strip())
        if m and not m.group(2).startswith("sub_") and m.group(1) == "T":
            strong.add(m.group(2))
    return strong


STATUSES = ("generated", "displaced", "dropped")


def pct(part, whole):
    return f"{100.0 * part / whole:.1f}%" if whole else "-"


def guest_modules(port, symdoc):
    """[(name, ranges, policy)] from native.toml, then ("all", None, None)."""
    mods = []
    if load_native_toml is not None:
        try:
            for name, m in load_native_toml(port).items():
                mods.append((name, m["ranges"], m["policy"]))
        except (ValueError, OSError) as exc:
            print(f"native_report: native.toml ignored: {exc}", file=sys.stderr)
    mods.append(("all", None, None))
    return mods


def guest_section(port, symdoc, reach, perf, reach_path, perf_path, top):
    """Markdown for the Guest functions section, plus a machine-readable summary
    dict per module (used by native_targets.py through --json-summary)."""
    symbols = symdoc["symbols"]
    lines, summary = [], {}
    lines.append("\n## Guest functions\n\n")
    lines.append(f"Recompiled guest functions by link status (`out/native/symbols.json`, "
                 f"{len(symbols)} functions from `{os.path.basename(str(symdoc.get('binary', '?')))}`).")
    if reach is not None:
        lines.append(f" Calls from `{reach_path}` ({len(reach)} reached rows).")
    else:
        lines.append(" No reach trace given (`--reach`), so reached columns are `-`; "
                     "build with `-DREXGLUE_GUEST_REACH=ON` and run with `REX_TRACE_REACH=<csv>`.")
    if perf is not None:
        lines.append(f" CPU samples from `{perf_path}` (`tools/reach_from_perf.sh`).")
    lines.append("\n\n")
    lines.append("A module is native only when every function in its range is displaced or dropped "
                 "(`native_check.py`, static) and its generated share of calls and CPU is 0.\n\n")

    hdr = "| module | policy | functions | generated | displaced | dropped | reached | reached generated | calls in generated | CPU in generated |\n"
    lines.append(hdr + "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n")
    details = []
    for name, ranges, policy in guest_modules(port, symdoc):
        members = [a for a in symbols if ranges is None or in_ranges(a, ranges)]
        by = {s: [a for a in members if symbols[a]["status"] == s] for s in STATUSES}
        row = {"functions": len(members), **{s: len(by[s]) for s in STATUSES}}
        row["policy"] = policy
        if reach is not None:
            reached = [a for a in members if reach.get(a, 0) > 0]
            calls_total = sum(reach.get(a, 0) for a in members)
            calls_gen = sum(reach.get(a, 0) for a in by["generated"])
            row.update(reached=len(reached), reached_generated=sum(1 for a in reached if symbols[a]["status"] == "generated"),
                       calls_total=calls_total, calls_generated=calls_gen)
        if perf is not None:
            cpu_total = sum(perf.get(a, 0) for a in members)
            cpu_gen = sum(perf.get(a, 0) for a in by["generated"])
            row.update(cpu_total=cpu_total, cpu_generated=cpu_gen)
        summary[name] = row
        rs = "-" if reach is None else str(row["reached"])
        rg = "-" if reach is None else str(row["reached_generated"])
        cw = "-" if reach is None else f"{pct(row['calls_generated'], row['calls_total'])} ({row['calls_generated']})"
        pw = "-" if perf is None else f"{pct(row['cpu_generated'], row['cpu_total'])} ({row['cpu_generated']})"
        rng = "" if ranges is None else " `" + ", ".join(f"{lo:08X}-{hi:08X}" for lo, hi in ranges) + "`"
        lines.append(f"| {name}{rng} | {policy or '-'} | {len(members)} | {len(by['generated'])} | {len(by['displaced'])} "
                     f"| {len(by['dropped'])} | {rs} | {rg} | {cw} | {pw} |\n")

        # Hot list: what to displace next in this module, by CPU then calls.
        weight = perf if perf is not None else reach
        if weight is not None and top > 0:
            hot = sorted((a for a in by["generated"] if weight.get(a, 0) > 0), key=lambda a: -weight[a])[:top]
            if hot:
                unit = "samples" if perf is not None else "calls"
                details.append(f"\n### {name}: hottest still-generated functions (by {unit})\n\n")
                details.append(f"| function | {unit} | calls |\n|---|---:|---:|\n")
                for a in hot:
                    calls = reach.get(a, 0) if reach is not None else "-"
                    details.append(f"| `sub_{a}` | {weight[a]} | {calls} |\n")
    lines += details
    if perf is not None:
        # guest share of the whole process, from the converter's trailer if present
        try:
            trailer = [l for l in open(perf_path, errors="ignore") if l.startswith("# samples=")]
            if trailer:
                m = re.search(r"samples=(\d+).*other_samples=(\d+)", trailer[-1])
                if m:
                    tot, other = int(m.group(1)), int(m.group(2))
                    lines.append(f"\nProcess CPU: {tot} samples, {pct(tot - other, tot)} inside guest functions "
                                 f"(the rest is runtime, kernel HLE and libraries).\n")
        except OSError:
            pass
    return "".join(lines), summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--trace", help="file from REX_TRACE_IMPORTS")
    ap.add_argument("--binary", help="linked title binary, to verify from the image")
    ap.add_argument("--write", action="store_true", help="write NATIVE_COVERAGE.md into the port")
    ap.add_argument("--symbols", help="symbols.json from native_map.py (default <port>/out/native/symbols.json)")
    ap.add_argument("--reach", help="REX_TRACE_REACH CSV (default <port>/out/native/reach.csv if present)")
    ap.add_argument("--perf", help="CPU-sample CSV from reach_from_perf.sh (default <port>/out/native/reach_perf.csv if present)")
    ap.add_argument("--top", type=int, default=15, help="hottest still-generated functions listed per module")
    ap.add_argument("--json-summary", help="also write the guest-function summary as json (native_targets.py reads out/native/guest_summary.json)")
    args = ap.parse_args()

    port = pathlib.Path(args.port).resolve()
    slug = port.name.replace("-recomp", "")

    imported = set()
    for init in (port / "generated" / "default").glob("*_init.cpp"):
        for m in re.finditer(r"__imp__([A-Za-z][A-Za-z0-9_]*)", init.read_text(encoding="utf-8", errors="replace")):
            if not m.group(1).startswith("sub_"):
                imported.add(m.group(1))

    called = set()
    if args.trace and pathlib.Path(args.trace).is_file():
        called = {l.strip().replace("__imp__", "") for l in open(args.trace) if l.strip()}
    target = called or imported
    label = "called at runtime" if called else "imported (no trace given)"

    native = native_names(port)
    strong = linked_strong(args.binary) if args.binary else None
    if strong is not None:
        # The image is the authority: a hook that did not actually displace the
        # SDK's weak definition is not native, whatever the sources say.
        claimed_not_linked = native - strong
        native = native & strong
    else:
        claimed_not_linked = set()

    done = sorted(native & target)
    todo = sorted(target - native)
    pct = 100 * len(done) / len(target) if target else 0

    lines = []
    lines.append(f"# {slug} - native kernel coverage\n")
    lines.append(f"{len(done)} of {len(target)} imports {label} are native "
                 f"({pct:.0f}%). {len(imported)} imported in total.\n")
    if claimed_not_linked:
        lines.append(f"\n**Declared but not linked:** {', '.join(sorted(claimed_not_linked))} - "
                     f"the SDK's weak definition is still the one in the binary.\n")
    lines.append("\n| group | native | remaining |\n|---|---:|---:|\n")
    groups = {}
    for n in target:
        g = categorise(n)
        groups.setdefault(g, [0, 0])
        groups[g][0 if n in native else 1] += 1
    for g in sorted(groups, key=lambda g: -sum(groups[g])):
        lines.append(f"| {g} | {groups[g][0]} | {groups[g][1]} |\n")
    if todo:
        lines.append("\n## Still ReXGlue\n\n")
        by_group = {}
        for n in todo:
            by_group.setdefault(categorise(n), []).append(n)
        for g in sorted(by_group, key=lambda g: -len(by_group[g])):
            lines.append(f"- **{g}** ({len(by_group[g])}): {', '.join(by_group[g])}\n")
    # ---- guest functions -------------------------------------------------
    native_dir = port / "out" / "native"
    sym_path = pathlib.Path(args.symbols) if args.symbols else native_dir / "symbols.json"
    reach_path = args.reach or (str(native_dir / "reach.csv") if (native_dir / "reach.csv").is_file() else None)
    perf_path = args.perf or (str(native_dir / "reach_perf.csv") if (native_dir / "reach_perf.csv").is_file() else None)
    summary = None
    if load_symbols is not None and sym_path.is_file():
        symdoc = load_symbols(sym_path)
        reach = load_reach(reach_path) if reach_path else None
        perf = load_reach(perf_path) if perf_path else None
        section, summary = guest_section(port, symdoc, reach, perf, reach_path, perf_path, args.top)
        lines.append(section)
    else:
        lines.append("\n## Guest functions\n\nNot mapped: run `tools/native_map.py <port>` "
                     "(needs a linked binary) to produce `out/native/symbols.json`.\n")

    report = "".join(lines)
    print(report)
    if args.write:
        out = port / "NATIVE_COVERAGE.md"
        out.write_text(report, encoding="utf-8")
        print(f"-> {out}")
    json_out = args.json_summary or (str(native_dir / "guest_summary.json") if args.write and summary is not None else None)
    if json_out and summary is not None:
        import json
        os.makedirs(os.path.dirname(json_out), exist_ok=True)
        with open(json_out, "w") as fh:
            json.dump({"format": "rex-native-guest-summary/1", "port": str(port.resolve()),
                       "reach": reach_path, "perf": perf_path, "modules": summary}, fh, indent=1)
            fh.write("\n")
        print(f"-> {json_out}")


if __name__ == "__main__":
    main()
