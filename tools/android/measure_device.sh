#!/usr/bin/env bash
# Measure a port ON THE DEVICE, and A/B its settings there.
#
# Why this exists: every performance number in this estate was taken on the
# desktop. Not one frame-stats CSV had ever been pulled off a handheld, so the
# reason a title is slow on Android had only ever been REASONED about - and the
# one team that did measure Geometry Wars on an Adreno 650 with hardware
# counters found the popular explanation (render-pass breaks) bought nothing
# when they fixed it. Guessing here is expensive; measuring is minutes.
#
# It also closes a second gap. Desktop reads config/<slug>.toml from beside the
# binary; Android has no such path, so a port's tuned settings NEVER reach the
# device unless something writes them into files/rexglue.args. Nothing did. So
# every Android run to date has been on defaults - including the settings that
# were worth 12->20 fps on desktop.
#
#   measure_device.sh <slug> [seconds] [--tuned] [--extra "--flag=v ..."]
#
#   --tuned   also push the port's own config/<slug>.toml as CLI flags, so the
#             device runs what the desktop runs. Without it, you measure defaults.
#
# Prints medians over the last 300 frames and leaves the CSV in the port's
# out/android/ for a diff.
set -uo pipefail

SLUG="${1:?usage: measure_device.sh <slug> [seconds] [--tuned] [--extra \"flags\"]}"
shift
SECS=60
TUNED=0
EXTRA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --tuned) TUNED=1; shift ;;
    --extra) EXTRA="${2:-}"; shift 2 ;;
    *[0-9]*) SECS="$1"; shift ;;
    *) shift ;;
  esac
done

SDK="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT="${PORT_ROOT:-$HOME/recomp-ports/recomp-family/$SLUG-recomp}"
PKG="com.crownpark.rexglue.$SLUG"
DEV="/sdcard/Android/data/$PKG/files"
# The tag has to distinguish every variant, or a run overwrites the baseline it
# is supposed to be compared against - which is how an A/B silently becomes an
# A/A.
TAG=$([ $TUNED -eq 1 ] && echo tuned || echo base)
[ -n "$EXTRA" ] && TAG="${TAG}_$(printf '%s' "$EXTRA" | tr -cd 'a-z0-9' | cut -c1-24)"
OUT="$PORT/out/android"
mkdir -p "$OUT"

command -v adb >/dev/null || { echo "adb not on PATH" >&2; exit 2; }
ADB=(adb)
[ -n "${ANDROID_SERIAL:-}" ] && ADB+=(-s "$ANDROID_SERIAL")
"${ADB[@]}" shell "pm list packages" 2>/dev/null | grep -q "^package:$PKG$" || {
  echo "$PKG is not installed on the device" >&2; exit 2; }

# ---- build the args file -------------------------------------------------
ARGS="$(mktemp)"; trap 'rm -f "$ARGS"' EXIT
echo "--gpu_frame_stats_path=$DEV/frames_$TAG.csv" > "$ARGS"

if [ $TUNED -eq 1 ]; then
  CFG="$PORT/config/$SLUG.toml"
  [ -f "$CFG" ] || { echo "no $CFG to apply" >&2; exit 2; }
  # Only scalar perf/behaviour cvars. Titles and paths are skipped: they carry
  # spaces, and gpu_plugin especially must never be repeated - a duplicate
  # blanks the value and the app comes up with no GPU plugin and a black screen.
  grep -E '^[a-z_]+ *= *(true|false|[0-9]+|"[a-z_]+")' "$CFG" 2>/dev/null \
    | grep -vE '^(window_title|side_panel_title|gpu_plugin|headless)' \
    | sed -E 's/ *= */=/; s/"//g; s/^/--/' >> "$ARGS"
fi
# --gpu_plugin cannot be set here. The activity bakes in the FIRST entry of
# GPU_PLUGINS at package time, and a second one in rexglue.args does not
# override it - it blanks it, and the app comes up with "no GPU emulation
# loaded (gpu_plugin not set)" and renders nothing. Repackage instead.
if printf '%s' "$EXTRA" | grep -q 'gpu_plugin'; then
  echo "refusing --gpu_plugin in --extra: it blanks the value at runtime." >&2
  echo "  rebuild with GPU_PLUGINS=\"<plugin> ...\" - the first entry is the default." >&2
  exit 2
fi
[ -n "$EXTRA" ] && printf '%s\n' $EXTRA >> "$ARGS"

echo "== $SLUG on device ($TAG) =="
sed 's/^/   /' "$ARGS"

# ---- run -----------------------------------------------------------------
# A sleeping device renders nothing and the CSV stays empty, which reads
# exactly like a broken build.
"${ADB[@]}" shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
"${ADB[@]}" shell "svc power stayon usb" >/dev/null 2>&1
# Stop first. Deleting the CSV while the previous instance still holds it lets
# that process recreate it, and the next run then APPENDS with no header row -
# the stats parse silently finds nothing and reports "no frames".
"${ADB[@]}" shell "am force-stop $PKG" >/dev/null 2>&1
"${ADB[@]}" shell "rm -f $DEV/frames_$TAG.csv" >/dev/null 2>&1
"${ADB[@]}" push "$ARGS" "$DEV/rexglue.args" >/dev/null 2>&1
"${ADB[@]}" shell "am start -n $PKG/.MainActivity" >/dev/null 2>&1

echo "   running ${SECS}s - PLAY IT: a menu is not a measurement"
for _ in $(seq 1 $((SECS / 3))); do
  sleep 3
  n=$("${ADB[@]}" shell "wc -l < $DEV/frames_$TAG.csv 2>/dev/null" 2>/dev/null | tr -dc '0-9')
  printf '\r   frames: %s   ' "${n:-0}"
done
echo

"${ADB[@]}" pull "$DEV/frames_$TAG.csv" "$OUT/frames_$TAG.csv" >/dev/null 2>&1 || {
  echo "no CSV produced - was the screen on, and did the title present?" >&2; exit 1; }

# ---- report --------------------------------------------------------------
python3 - "$OUT/frames_$TAG.csv" "$TAG" <<'PY'
import csv, statistics as st, sys
path, tag = sys.argv[1], sys.argv[2]
rows = [r for r in csv.DictReader(open(path)) if r.get('frame_ms')]
if not rows:
    print("  no frames"); raise SystemExit(1)
# Busy frames only. A title sitting on a menu draws tens of primitives and
# reports 60fps on hardware that cannot hold 30 in a level.
draws = sorted(float(r['draws']) for r in rows)
busy = [r for r in rows if float(r['draws']) >= draws[len(draws)//2]]
sel = (busy or rows)[-300:]
def med(k):
    v = [float(r[k]) for r in sel if r.get(k) not in (None, '')]
    return st.median(v) if v else 0.0
fm = med('frame_ms')
print(f"  {tag}: {len(rows)} frames, {len(sel)} busy analysed")
print(f"    frame_ms       {fm:8.2f}  -> {1000/max(fm,0.01):5.1f} fps")
for k in ('draws','render_passes','rp_breaks_barrier','resolves','upload_pages',
          'upload_ms','draw_cpu_ms','vbuffers_ms','vfetch_ms','b_rt','b_shmem'):
    print(f"    {k:<14} {med(k):8.2f}")
PY

echo "   CSV: $OUT/frames_$TAG.csv"
echo
echo "   A/B:  $0 $SLUG $SECS            # defaults"
echo "         $0 $SLUG $SECS --tuned    # the port's own config"
