#!/usr/bin/env python3
"""Compare two tee digest logs (REX_TEE_LOG output) and report the first divergence.

    tools/tee_diff.py a.jsonl b.jsonl [--all] [--keys REGEX] [--offset N]
                      [--frames LO-HI] [--json out.json]

Each log is JSON lines {"frame": N, "key": "chan/key", "hash": "16 hex", "n": count}
as written by rex::tee::Channel (format: docs/native/FORMATS.md, "Tee digest").
Frames are compared in ascending order over the frames present in BOTH logs;
within a frame, keys in sorted order. The first (frame, key) whose hash differs
or that is present on one side only is the divergence. --offset N aligns B to A
(B's frame N+k is compared with A's frame k) for runs whose frame counters start
apart. --all lists every divergence instead of stopping at the first. Exit status
0 when the compared frames agree, 1 on divergence, 2 on bad input.
"""
import argparse
import json
import re
import sys
from collections import defaultdict


def load(path):
    frames = defaultdict(dict)  # frame -> key -> (hash, n)
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
                frames[int(rec["frame"])][rec["key"]] = (rec["hash"], int(rec.get("n", 0)))
            except (ValueError, KeyError) as e:
                print(f"{path}:{lineno}: bad line ({e})", file=sys.stderr)
                return None
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--all", action="store_true", help="list every divergence, not just the first")
    ap.add_argument("--keys", help="regex; only keys matching it are compared")
    ap.add_argument("--offset", type=int, default=0, help="B frame = A frame + offset")
    ap.add_argument("--frames", help="LO-HI (A frame numbers, inclusive) to compare")
    ap.add_argument("--json", help="write a machine-readable summary here")
    args = ap.parse_args()

    a, b = load(args.a), load(args.b)
    if a is None or b is None:
        return 2
    key_re = re.compile(args.keys) if args.keys else None
    lo, hi = 0, 1 << 62
    if args.frames:
        lo, hi = (int(x) for x in args.frames.split("-", 1))

    common = sorted(f for f in a if lo <= f <= hi and (f + args.offset) in b)
    only_a = sorted(f for f in a if (f + args.offset) not in b)
    only_b = sorted(f - args.offset for f in b if (f - args.offset) not in a)

    divergences = []
    keys_compared = 0
    for fa in common:
        ka, kb = a[fa], b[fa + args.offset]
        for key in sorted(set(ka) | set(kb)):
            if key_re and not key_re.search(key):
                continue
            if key not in ka:
                divergences.append((fa, key, None, kb[key]))
            elif key not in kb:
                divergences.append((fa, key, ka[key], None))
            else:
                keys_compared += 1
                if ka[key][0] != kb[key][0]:
                    divergences.append((fa, key, ka[key], kb[key]))
        if divergences and not args.all:
            break

    summary = {
        "a": args.a, "b": args.b, "offset": args.offset,
        "frames_a": len(a), "frames_b": len(b), "frames_compared": len(common),
        "frames_only_a": len(only_a), "frames_only_b": len(only_b),
        "keys_compared": keys_compared, "divergences": len(divergences),
        "first": None,
    }
    if divergences:
        fa, key, ha, hb = divergences[0]
        summary["first"] = {"frame": fa, "key": key,
                            "a": ha and {"hash": ha[0], "n": ha[1]},
                            "b": hb and {"hash": hb[0], "n": hb[1]}}
    if args.json:
        with open(args.json, "w") as f:
            json.dump(summary, f, indent=1)

    print(f"frames: A {len(a)}  B {len(b)}  compared {len(common)}"
          f"  only-A {len(only_a)}  only-B {len(only_b)}  keys compared {keys_compared}")
    if not common:
        print("no common frames (try --offset)")
        return 1
    if not divergences:
        print(f"MATCH over frames {common[0]}..{common[-1]}")
        return 0

    def fmt(h):
        return "missing" if h is None else f"{h[0]} (n={h[1]})"
    fa, key, ha, hb = divergences[0]
    print(f"FIRST DIVERGENCE at frame {fa} key {key}: A {fmt(ha)}  B {fmt(hb)}")
    if args.all:
        print(f"{len(divergences)} divergences:")
        for fa, key, ha, hb in divergences[:200]:
            print(f"  frame {fa:6d} {key:32} A {fmt(ha)}  B {fmt(hb)}")
        if len(divergences) > 200:
            print(f"  ... {len(divergences) - 200} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())
