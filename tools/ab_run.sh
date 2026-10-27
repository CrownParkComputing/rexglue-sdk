#!/usr/bin/env bash
# A/B two configurations of one port through its headless harness and print
# one table: tee digest divergence, frame glitch/PSNR stats (when the port has
# tools/d3d/ab_frames.sh) and per-frame cost medians (the same fields as
# tools/measure.sh, computed from the frame-stats CSV of each run so the game
# runs exactly twice).
#
#   tools/ab_run.sh <port_dir> <secs> "<key script>" "<envA>" "<envB>" [labelA] [labelB]
#
#   port_dir   a port with tools/headless_play.sh
#   secs       seconds per run
#   key script headless_play.sh script, e.g. "25:Return 100:e:35"
#   envA/envB  space-separated VAR=VALUE words passed through GAME_ENV; cvars
#              go in AB_ARGS_A / AB_ARGS_B (appended to GAME_ARGS), e.g.
#              AB_ARGS_B="--pgr4_d3d=tee". An empty string is a valid side.
#
# Outputs under <port_dir>/out/ab/<label>/: run.log, shots/, tee.jsonl,
# frames.csv; the comparison in <port_dir>/out/ab/tee_diff.json and
# ab_run.md (the table). Environment knobs: AB_OUT (output root),
# AB_HEADLESS (alternative harness script, for tests), AB_TEE_OPTS (extra
# tee_diff.py options, e.g. "--offset 3"), AB_SKIP_FRAMES=1, AB_KEEP=1 (do not
# clear previous outputs). Exit status: 0 when the tee digests match (or no
# tee log was produced on either side), 1 on tee divergence, 2 on usage.
set -u
usage() { echo "usage: $0 <port_dir> <secs> \"<key script>\" \"<envA>\" \"<envB>\" [labelA] [labelB]" >&2; exit 2; }
[ $# -ge 5 ] || usage
PORT="$1"; SECS="$2"; SCRIPT="$3"; ENV_A="$4"; ENV_B="$5"; LA="${6:-A}"; LB="${7:-B}"
[ -d "$PORT" ] || { echo "ab_run: $PORT is not a directory" >&2; usage; }
case "$SECS" in ''|*[!0-9]*) echo "ab_run: secs must be an integer" >&2; usage;; esac
[ "$LA" != "$LB" ] || { echo "ab_run: labels must differ" >&2; usage; }
PORT="$(cd -- "$PORT" && pwd)"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
HEADLESS="${AB_HEADLESS:-$PORT/tools/headless_play.sh}"
[ -x "$HEADLESS" ] || { echo "ab_run: $HEADLESS is not executable" >&2; exit 2; }
TEE_DIFF="$HERE/tee_diff.py"
AB_FRAMES="$PORT/tools/d3d/ab_frames.sh"
ROOT="${AB_OUT:-$PORT/out/ab}"
mkdir -p "$ROOT"

run_side() {  # label env args
  local L="$1" E="$2" ARGS="$3" OUT="$ROOT/$1"
  [ -n "${AB_KEEP:-}" ] || rm -rf "$OUT"
  mkdir -p "$OUT"
  echo "== $L: env [$E] args [$ARGS] ($SECS s) -> $OUT" >&2
  local T0=$SECONDS
  HEADLESS_OUT="$OUT" \
  GAME_ENV="$E REX_TEE_LOG=$OUT/tee.jsonl" \
  GAME_ARGS="--gpu_frame_stats_path=$OUT/frames.csv $ARGS" \
    "$HEADLESS" "$SECS" "$SCRIPT" >"$OUT/headless.log" 2>&1
  echo "$((SECONDS - T0))" >"$OUT/wall_s"
}

run_side "$LA" "$ENV_A" "${AB_ARGS_A:-}"
run_side "$LB" "$ENV_B" "${AB_ARGS_B:-}"
OA="$ROOT/$LA"; OB="$ROOT/$LB"

# Per-side summary: shots, tee lines, frame-stats medians (measure.sh fields).
side_row() {  # label env args
  local OUT="$ROOT/$1"
  python3 - "$1" "$2 ${3:+/ $3}" "$OUT" <<'PY'
import csv, glob, os, statistics, sys
label, cfg, out = sys.argv[1], sys.argv[2].strip() or "(default)", sys.argv[3]
shots = len(glob.glob(os.path.join(out, "shots", "*.png")))
tee = sum(1 for _ in open(os.path.join(out, "tee.jsonl"))) if os.path.exists(os.path.join(out, "tee.jsonl")) else 0
wall = open(os.path.join(out, "wall_s")).read().strip() if os.path.exists(os.path.join(out, "wall_s")) else "-"
frame = draws = dcpu = res = up = "-"
p = os.path.join(out, "frames.csv")
if os.path.exists(p):
    rows = list(csv.DictReader(open(p)))
    tail = [r for r in rows if r.get("draws") and float(r["draws"]) > 1000][-80:]
    if tail:
        def md(k):
            v = [float(r[k]) for r in tail if r.get(k)]
            return statistics.median(v) if v else 0.0
        frame = f"{md('frame_ms'):.1f} ms ({1000 / max(md('frame_ms'), 1e-9):.1f} FPS)"
        draws = f"{md('draws'):.0f}"; dcpu = f"{md('draw_cpu_ms'):.1f}"; res = f"{md('vfetch_ms'):.1f}"
        up = f"{md('upload_pages') * 4 / 1024:.1f} MB"
print(f"| {label} | {cfg} | {wall} | {shots} | {tee} | {frame} | {draws} | {dcpu} | {res} | {up} |")
PY
}

TABLE="$ROOT/ab_run.md"
TEE_RC=0
{
  echo "### ab_run: $(basename "$PORT"), ${SECS}s, script \"$SCRIPT\""
  echo
  echo "| run | config | wall s | shots | tee lines | frame (median, last 80) | draws | draw cpu ms | residency ms | uploads |"
  echo "|---|---|---|---|---|---|---|---|---|---|"
  side_row "$LA" "$ENV_A" "${AB_ARGS_A:-}"
  side_row "$LB" "$ENV_B" "${AB_ARGS_B:-}"
  echo

  # Tee digests.
  TEE_RC=0
  if [ -s "$OA/tee.jsonl" ] && [ -s "$OB/tee.jsonl" ]; then
    TEE_OUT=$(python3 "$TEE_DIFF" "$OA/tee.jsonl" "$OB/tee.jsonl" --json "$ROOT/tee_diff.json" ${AB_TEE_OPTS:-} 2>&1); TEE_RC=$?
    echo "| tee digest | $(echo "$TEE_OUT" | tail -1) |"
    echo "|---|---|"
    echo "| $(echo "$TEE_OUT" | head -1 | sed 's/  */ /g') | tools/tee_diff.py exit $TEE_RC |"
  else
    echo "| tee digest | no REX_TEE_LOG output on $([ -s "$OA/tee.jsonl" ] || echo "$LA ")$([ -s "$OB/tee.jsonl" ] || echo "$LB") (no tee channel published) |"
    echo "|---|---|"
  fi
  echo

  # Frames: the port's own A/B (triangle detector + PSNR/SSIM), when present.
  if [ -z "${AB_SKIP_FRAMES:-}" ] && [ -x "$AB_FRAMES" ]; then
    "$AB_FRAMES" "$OA" "$OB" "$LA" "$LB" 2>"$ROOT/ab_frames.err" || true
  elif [ -z "${AB_SKIP_FRAMES:-}" ]; then
    echo "(no tools/d3d/ab_frames.sh in this port: frame comparison skipped)"
  fi
  echo
  [ -x "$PORT/tools/measure.sh" ] && echo "(per-frame medians above use the measure.sh fields from each run's frames.csv)"
} >"$TABLE"
cat "$TABLE"
exit "${TEE_RC:-0}"
