#!/usr/bin/env bash
# Build host-decoded YUV references once after importing the game's WMV files.
# The recompiled guest decoder damages Y/U/V pixels before texture upload.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_ROOT="$ROOT/assets"
OUT="$ROOT/out/fmvref"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --root|--output)
      [ "$#" -ge 2 ] || { echo "Missing path after $1" >&2; exit 2; }
      if [ "$1" = --root ]; then GAME_ROOT="$2"; else OUT="$2"; fi
      shift 2 ;;
    --help)
      echo "usage: prepare_fmv.sh [--root extracted-disc-1] [--output video-cache]"
      exit 0 ;;
    *) echo "Unknown video preparation option: $1" >&2; exit 2 ;;
  esac
done
mkdir -p "$OUT"
command -v ffmpeg >/dev/null || { echo "ffmpeg is required for clean Forza videos" >&2; exit 1; }
for name in t10_ms_combined dolby_corona_intro forza_intro IE_Welcome_Video; do
  if [ -f "$GAME_ROOT/Media/UI/videos/Splash_Intros/$name.wmv" ]; then
    source="$GAME_ROOT/Media/UI/videos/Splash_Intros/$name.wmv"
  else
    source="$GAME_ROOT/Media/UI/videos/$name.wmv"
  fi
  [ -f "$source" ] || { echo "Forza video missing: $name.wmv" >&2; exit 1; }
  target="$OUT/$name.yuv"
  # 1280x720 yuv420p: 1,382,400 bytes per frame. A partial decode is never
  # accepted, and an interrupted launch can safely retry.
  if [ -f "$target" ] && [ "$(stat -c %s "$target")" -gt 0 ] &&
     [ "$(( $(stat -c %s "$target") % 1382400 ))" -eq 0 ] &&
     [ "$target" -nt "$source" ]; then
    continue
  fi
  temp="$target.tmp.$$"
  trap 'rm -f "$temp"' EXIT
  echo "Preparing Forza video: $name" >&2
  ffmpeg -nostdin -v error -i "$source" -an -pix_fmt yuv420p -f rawvideo "$temp"
  [ "$(stat -c %s "$temp")" -gt 0 ] || { echo "Empty video decode: $name" >&2; exit 1; }
  mv -f "$temp" "$target"
  trap - EXIT
done
printf '%s\n' "$OUT"
