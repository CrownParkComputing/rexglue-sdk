#!/usr/bin/env python3
"""Score how much of a port's kernel surface is native rather than ReXGlue.

  native_report.py <port-dir> [--trace used.txt] [--binary path] [--write]

Two numbers matter and they are different:

  * IMPORTED  - what the XEX lists. Includes plenty the title never calls.
  * CALLED    - what it actually reached at runtime, from a trace produced by
                REX_TRACE_IMPORTS=<file> ./run.sh. This is the real work list.

Coverage is measured against CALLED. Kernel exports in the SDK are weak, so a
REX_NATIVE_HOOK in the port's own sources displaces one; --binary confirms that
from the linked image (T = the port's, W = still ReXGlue's) rather than trusting
the source scan.
"""
import argparse
import pathlib
import re
import subprocess
import sys

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--trace", help="file from REX_TRACE_IMPORTS")
    ap.add_argument("--binary", help="linked title binary, to verify from the image")
    ap.add_argument("--write", action="store_true", help="write NATIVE_COVERAGE.md into the port")
    args = ap.parse_args()

    port = pathlib.Path(args.port)
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
    report = "".join(lines)
    print(report)
    if args.write:
        out = port / "NATIVE_COVERAGE.md"
        out.write_text(report, encoding="utf-8")
        print(f"-> {out}")


if __name__ == "__main__":
    main()
