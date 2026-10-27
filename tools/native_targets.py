#!/usr/bin/env python3
"""Every kernel import the estate actually calls, and what it would cost to
make it native. The list you pick a job off.

  native_targets.py                     ranked work list across all ports
  native_targets.py --group "file I/O"  one group, with the ports that need it
  native_targets.py --port segarally    one port's remaining surface
  native_targets.py --brief             just the headline numbers

Ranking is by **how many ports call it**, because a kernel export is written
once in the SDK-facing sense but displaced per port by a REX_NATIVE_HOOK, and
an import that eleven titles reach is eleven titles moved by one piece of
understanding. An import only one title calls is worth less even if it looks
easy.

CALLED, not IMPORTED. A port with a trace (out/native/used.txt, produced by
pipeline.py run trace) contributes the set it really reaches; the XEX table
overstates it badly - Sega Rally imports 347 and calls 123. Ports without a
trace are listed separately rather than folded in, so the numbers stay honest.

Guest code (the second axis, from native_report.py --write, which leaves
out/native/guest_summary.json beside symbols.json): per port, how many
recompiled functions are still generated and - when a REX_TRACE_REACH /
reach_from_perf.sh CSV fed the report - what share of calls and CPU still
runs in them, per native.toml module. Modules are ranked by CPU share in
generated code (calls when no perf CSV), because a module that owns 40% of
the frame is worth more than one with more functions.

  native_targets.py --guest          only the guest-code ranking
"""
import argparse
import collections
import json
import pathlib
import re
import sys

PORTS_ROOT = pathlib.Path("/home/jon/recomp-ports")
# The recomp-family collections keep ports two levels under collections/.
FAMILY_GLOBS = ("recomp-family/collections/*/*/*-recomp",)

# Same grouping as native_report.py, plus a rough difficulty. "easy" means an
# ordinary host facility with no console semantics; "hard" means it encodes
# something about the 360 that has to be understood first.
GROUPS = [
    ("threads / TLS",   "easy", lambda n: n.startswith(("ExCreateThread", "ExTerminateThread", "KeTls",
                                                        "KeSetAffinityThread", "KeSetBasePriorityThread",
                                                        "KeQueryBasePriorityThread", "NtResumeThread",
                                                        "NtSuspendThread", "Ob"))),
    ("sync / events",   "easy", lambda n: n.startswith(("KeWaitFor", "NtWaitFor", "NtCreateEvent", "NtSetEvent",
                                                        "NtClearEvent", "NtPulseEvent", "KeSetEvent", "KeResetEvent",
                                                        "NtCreateSemaphore", "NtReleaseSemaphore", "RtlEnterCritical",
                                                        "RtlLeaveCritical", "RtlInitializeCritical", "RtlTryEnterCritical",
                                                        "KfAcquireSpinLock", "KfReleaseSpinLock", "KeAcquireSpinLock",
                                                        "KeReleaseSpinLock"))),
    ("file I/O",        "easy", lambda n: n.startswith(("NtCreateFile", "NtReadFile", "NtWriteFile", "NtOpenFile",
                                                        "NtClose", "NtQueryInformationFile", "NtQueryDirectoryFile",
                                                        "NtQueryVolumeInformationFile", "NtSetInformationFile",
                                                        "NtQueryFullAttributes", "NtFlushBuffersFile"))),
    ("memory",          "easy", lambda n: n.startswith(("NtAllocateVirtualMemory", "NtFreeVirtualMemory",
                                                        "NtQueryVirtualMemory", "Mm"))),
    ("string / rtl",    "easy", lambda n: n.startswith(("Rtl", "sprintf", "vsprintf", "_vsnprintf", "sscanf"))),
    ("locale / system", "easy", lambda n: n.startswith(("XGetLanguage", "XGetAVPack", "XGetGameRegion",
                                                        "XGetVideoMode", "ExGetXConfigSetting"))),
    ("input",           "easy", lambda n: n.startswith("XamInput")),
    ("profile / saves", "hard", lambda n: n.startswith(("XamContent", "XamUser", "XamShow", "XamProfile"))),
    ("audio",           "hard", lambda n: n.startswith(("XMA", "XAudio"))),
    ("GPU / video",     "trap", lambda n: n.startswith("Vd")),
    ("networking",      "hard", lambda n: n.startswith(("NetDll", "XNet"))),
]


def group_of(name):
    for g, diff, pred in GROUPS:
        if pred(name):
            return g, diff
    return "other", "hard"


def native_names(port):
    """Imports this port has already displaced with its own definition."""
    out = set()
    src = port / "src"
    if src.is_dir():
        for f in list(src.rglob("*.cpp")) + list(src.rglob("*.h")):
            try:
                t = f.read_text(errors="ignore")
            except OSError:
                continue
            out |= set(re.findall(r"REX_NATIVE_HOOK\(\s*__imp__([A-Za-z0-9_]+)", t))
    return out


def all_ports(ports_root):
    # Dedup by real path (a top-level *-recomp may be a symlink into the
    # family tree); a slug that exists twice keeps the one with newer out/native.
    by_slug = {}
    for pat in ("*-recomp",) + FAMILY_GLOBS:
        for port in sorted(ports_root.glob(pat)):
            if not port.is_dir():
                continue
            real = port.resolve()
            prev = by_slug.get(port.name)
            if prev is None or prev.resolve() == real:
                by_slug.setdefault(port.name, port)
                continue
            def stamp(p):
                d = p / "out" / "native"
                return max((f.stat().st_mtime for f in d.glob("*") if f.is_file()), default=0)
            if stamp(port) > stamp(prev):
                by_slug[port.name] = port
    return [by_slug[k] for k in sorted(by_slug)]


def load_guest(ports_root):
    """{slug: {"modules": {name: row}, "reach": path|None, "perf": path|None}}
    from out/native/guest_summary.json (native_report.py --write)."""
    out = {}
    for port in all_ports(ports_root):
        p = port / "out" / "native" / "guest_summary.json"
        if not p.is_file():
            continue
        try:
            doc = json.loads(p.read_text(errors="ignore"))
        except ValueError:
            continue
        out[port.name.replace("-recomp", "")] = doc
    return out


def guest_weight(row):
    """(weight 0..1, basis) for a module row: CPU share in generated code when
    perf data exists, else call share, else the function share."""
    if row.get("cpu_total"):
        return row["cpu_generated"] / row["cpu_total"], "cpu"
    if row.get("calls_total"):
        return row["calls_generated"] / row["calls_total"], "calls"
    return (row["generated"] / row["functions"] if row.get("functions") else 0.0), "functions"


def print_guest(guest):
    print("-- guest code still generated, by module (rank: CPU share in generated code, else calls, else count)")
    print()
    if not guest:
        print("   no port has out/native/guest_summary.json yet")
        print("   Run:  python3 tools/native_map.py <port> && python3 tools/native_report.py <port> --write [--reach csv] [--perf csv]")
        print()
        return
    rows = []
    for slug, doc in guest.items():
        for name, row in doc.get("modules", {}).items():
            w, basis = guest_weight(row)
            rows.append((name == "all", -w, slug, name, row, basis))
    rows.sort()
    print(f"   {'port':<18}{'module':<12}{'policy':<14}{'gen/total':>12}  {'weight':>7}  basis")
    for _, negw, slug, name, row, basis in rows:
        gen = f"{row['generated']}/{row['functions']}"
        print(f"   {slug:<18}{name:<12}{(row.get('policy') or '-'):<14}{gen:>12}  {100*-negw:6.1f}%  {basis}")
    print()
    print("   weight = share of that module's CPU (or calls) still spent in recompiled bodies;")
    print("   a module is native at 0.0% with native_check.py passing. Untraced ports show 'functions'.")
    print()


def load(ports_root):
    traced, untraced = {}, []
    for port in all_ports(ports_root):
        t = port / "out" / "native" / "used.txt"
        if not t.is_file():
            untraced.append(port.name.replace("-recomp", ""))
            continue
        names = set()
        for line in t.read_text(errors="ignore").splitlines():
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            s = re.sub(r"^__imp__", "", s.split()[0])
            names.add(s)
        traced[port.name.replace("-recomp", "")] = (names, native_names(port))
    return traced, untraced


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", default=str(PORTS_ROOT))
    ap.add_argument("--group")
    ap.add_argument("--port")
    ap.add_argument("--brief", action="store_true")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--guest", action="store_true", help="only the guest-code ranking")
    a = ap.parse_args()

    guest = load_guest(pathlib.Path(a.ports))
    if a.guest:
        print_guest(guest)
        return

    traced, untraced = load(pathlib.Path(a.ports))
    if not traced:
        print_guest(guest)
        print("No port has an import trace yet.")
        print("Run:  python3 tools/pipeline.py <port> run trace")
        if untraced:
            print("Untraced ports: " + ", ".join(untraced))
        return

    # import -> ports calling it / ports where it is already native
    calls = collections.defaultdict(set)
    done = collections.defaultdict(set)
    for slug, (names, nat) in traced.items():
        for n in names:
            calls[n].add(slug)
            if n in nat:
                done[n].add(slug)

    total = len(calls)
    fully = sum(1 for n in calls if calls[n] == done[n])
    print("== native migration: the estate's real kernel surface")
    print(f"   traced ports   {len(traced)}: " + ", ".join(sorted(traced)))
    if untraced:
        print(f"   NOT traced     {len(untraced)}: " + ", ".join(untraced)
              + "   (run trace - they are not counted)")
    print(f"   distinct imports actually called   {total}")
    print(f"   already native everywhere          {fully}  ({100*fully//max(total,1)}%)")
    print()
    for slug, (names, nat) in sorted(traced.items()):
        pct = 100 * len(nat & names) // max(len(names), 1)
        g = ""
        row = guest.get(slug, {}).get("modules", {}).get("all")
        if row:
            w, basis = guest_weight(row)
            g = f"   guest {row['generated']}/{row['functions']} generated, {100*w:.1f}% by {basis}"
        print(f"   {slug:<18} {len(names):>4} called   {len(nat & names):>3} native  {pct:>3}%{g}")
    print()
    print_guest(guest)
    if a.brief:
        return

    rows = []
    for n, ports in calls.items():
        if n in done and done[n] == ports:
            continue
        g, diff = group_of(n)
        if a.group and g.lower() != a.group.lower():
            continue
        if a.port and a.port not in ports:
            continue
        rows.append((len(ports), g, diff, n, sorted(ports)))
    # most shared first, then easiest, then name
    order = {"easy": 0, "hard": 1, "trap": 2}
    rows.sort(key=lambda r: (-r[0], order.get(r[2], 3), r[3]))

    if a.group or a.port:
        title = a.group or a.port
        print(f"-- target: {title}   ({len(rows)} imports remaining)")
        print()
        for cnt, g, diff, n, ports in rows:
            print(f"   {n:<42} {cnt:>2} ports  [{diff}]  {','.join(ports)}")
        print()
        print("   Hand this to the agent as: \"go native on " + str(title) + "\".")
        return

    bygroup = collections.Counter()
    for cnt, g, diff, n, ports in rows:
        bygroup[(g, diff)] += 1
    print("-- remaining by group (pick one of these as a job)")
    print()
    print(f"   {'group':<20}{'left':>6}  difficulty")
    for (g, diff), cnt in sorted(bygroup.items(), key=lambda kv: (-kv[1], kv[0][0])):
        note = "  <- GPU setup only; the real work is the Xenos stream" if diff == "trap" else ""
        print(f"   {g:<20}{cnt:>6}  {diff}{note}")
    print()
    print(f"-- the {a.top} imports shared by the most titles (do these first)")
    print()
    for cnt, g, diff, n, ports in rows[:a.top]:
        print(f"   {n:<42} {cnt:>2} ports  {g:<18} [{diff}]")
    print()
    print("   Then:  native_targets.py --group \"file I/O\"")
    print("   and hand that to the agent as a job.")


if __name__ == "__main__":
    main()
