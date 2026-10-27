#!/usr/bin/env python3
"""Audit a captured gameplay scene for GPU errors and frame-time regressions.

Example: python3 tools/graphics_audit.py --log port/out/build/linux/logs/game.log \
  --stats /tmp/race.csv --min-draws 2000 --fps 60 --strict

This is a red-flag gate, not visual parity: pass it alongside oracle frame and
texture/resolve evidence described in docs/recomp-cookbook/70-port-acceptance.md.
"""
import argparse
import csv
import json
import re
import sys
from collections import Counter
from pathlib import Path

GPU_ERROR = re.compile(r"\[error\] \[gpu\].*?\] (.*)$", re.I)


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", type=Path, action="append", required=True,
                    help="game log, repeat for rotated logs")
    ap.add_argument("--stats", type=Path, required=True,
                    help="CSV written by --gpu_frame_stats_path")
    ap.add_argument("--min-draws", type=int, default=0,
                    help="minimum draw count for the gameplay scene")
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--json", type=Path, help="write machine-readable report")
    ap.add_argument("--strict", action="store_true",
                    help="fail on any GPU error, scene pipeline compile, or p99 over budget")
    args = ap.parse_args()
    if args.fps <= 0 or args.min_draws < 0:
        ap.error("--fps must be positive and --min-draws nonnegative")
    for path in [*args.log, args.stats]:
        if not path.is_file():
            ap.error(f"missing evidence: {path}")

    errors = Counter()
    for path in args.log:
        with path.open(errors="replace") as handle:
            for line in handle:
                match = GPU_ERROR.search(line)
                if match:
                    errors[match.group(1).strip()] += 1

    scene = []
    with args.stats.open(newline="") as handle:
        reader = csv.DictReader(handle)
        required = {"swap", "frame_ms", "draws", "pipelines_created", "pipeline_create_ms"}
        if not required.issubset(reader.fieldnames or []):
            ap.error(f"frame CSV lacks {', '.join(sorted(required - set(reader.fieldnames or [])))}")
        for row in reader:
            try:
                if int(row["draws"]) >= args.min_draws:
                    scene.append({key: float(row[key]) for key in required})
            except (ValueError, TypeError):
                continue  # Incomplete final row while a capture is still running.
    if not scene:
        ap.error("no captured frames match --min-draws")
    times = [row["frame_ms"] for row in scene]
    pipeline_frames = [row for row in scene if row["pipelines_created"] > 0]
    budget = 1000.0 / args.fps
    report = {
        "frame_count": len(scene), "min_draws": args.min_draws,
        "target_fps": args.fps, "frame_budget_ms": round(budget, 3),
        "frame_ms": {name: round(percentile(times, fraction), 3)
                     for name, fraction in (("p50", .5), ("p90", .9), ("p99", .99))},
        "max_frame_ms": round(max(times), 3),
        "new_pipelines": int(sum(row["pipelines_created"] for row in pipeline_frames)),
        "pipeline_create_ms": round(sum(row["pipeline_create_ms"] for row in pipeline_frames), 3),
        "worst_pipeline_frames": [
            {"swap": int(row["swap"]), "frame_ms": round(row["frame_ms"], 3),
             "created": int(row["pipelines_created"]),
             "create_ms": round(row["pipeline_create_ms"], 3)}
            for row in sorted(pipeline_frames, key=lambda row: row["frame_ms"], reverse=True)[:5]
        ],
        "gpu_error_count": sum(errors.values()),
        "gpu_errors": [{"message": message, "count": count}
                       for message, count in errors.most_common(12)],
    }
    report["pass"] = (report["gpu_error_count"] == 0 and
                      report["new_pipelines"] == 0 and
                      report["frame_ms"]["p99"] <= budget)
    output = json.dumps(report, indent=2)
    print(output)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(output + "\n")
    return 1 if args.strict and not report["pass"] else 0


if __name__ == "__main__":
    sys.exit(main())
