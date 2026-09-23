#!/usr/bin/env bash
# rayplay.sh - run a rexglue-vmx port with rayview as the live display.
#
#   rayplay.sh <project-dir> [seconds]
#
# The game runs its usual gamescope-headless harness (nothing in the runtime
# changes) with REX_DUMP_FRAME streaming PPMs; rayview tails the dump
# directory and is the window you watch and drive. Input comes from rayview's
# virtual Xbox pad (uinput), which SDL sees regardless of window focus.
#
# REX_DUMP_FRAME_EVERY=3 keeps the readback cost tolerable; raise it for
# timing-sensitive work.
set -euo pipefail

PROJECT="${1:?usage: rayplay.sh <project-dir> [seconds]}"
SECS="${2:-300}"
TITLE="$(basename "$PROJECT" -recomp)"
DUMP_DIR="/tmp/rayview-${TITLE}"
RAYVIEW="$(cd "$(dirname "$0")" && pwd)/build/rayview"

[ -x "$RAYVIEW" ] || { echo "build rayview first: cmake --build $(dirname "$0")/build"; exit 1; }
rm -rf "$DUMP_DIR" 2>/dev/null || true
if [ -e "$DUMP_DIR" ]; then DUMP_DIR="${DUMP_DIR}-$$"; fi
mkdir -p "$DUMP_DIR"

REX_DUMP_FRAME="${DUMP_DIR}/f" \
REX_DUMP_FRAME_EVERY="${REX_DUMP_FRAME_EVERY:-3}" \
REX_DUMP_FRAME_MAX=99999 \
"$PROJECT/tools/headless_play.sh" "$SECS" "" &
GAME_RUNNER=$!

cleanup() {
  kill "$GAME_RUNNER" 2>/dev/null || true
  pkill -f "build/linux/${TITLE}" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for the first frame so rayview does not open on an empty dir.
for _ in $(seq 1 100); do
  compgen -G "${DUMP_DIR}/f_*.ppm" >/dev/null && break
  sleep 0.3
done

"$RAYVIEW" "$DUMP_DIR" ${RAYVIEW_ARGS:-}
