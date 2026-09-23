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
"""
import argparse
import collections
import pathlib
import re
import sys

PORTS_ROOT = pathlib.Path("/home/jon/recomp-ports")

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


def load(ports_root):
    traced, untraced = {}, []
    for port in sorted(ports_root.glob("*-recomp")):
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
    a = ap.parse_args()

    traced, untraced = load(pathlib.Path(a.ports))
    if not traced:
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
        print(f"   {slug:<18} {len(names):>4} called   {len(nat & names):>3} native  {pct:>3}%")
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
