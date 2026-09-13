#!/usr/bin/env bash
# Pack the game content into one zip, and restore it.
#
#   tools/content_zip.sh pack      assets/ -> content/<title>-content.zip
#   tools/content_zip.sh restore   content/<title>-content.zip -> assets/
#   tools/content_zip.sh verify    check assets/ against the recorded checksums
#
# One archive, not a tree of loose disc files: it imports in a single step when
# rebuilding, and it never enters Git. content/content.sha256 (which IS in Git)
# records the archive's checksum and the file list, so a restored tree can be
# proven to be the right content without shipping the content itself.
#
# Stored, not deflated: disc data is already compressed, so deflate costs
# minutes of CPU to save a few per cent.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TITLE="$(basename "$ROOT" | sed 's/-recomp$//')"
ZIP="$ROOT/content/${TITLE}-content.zip"
SUMS="$ROOT/content/content.sha256"
ACTION="${1:-}"

case "$ACTION" in
  pack)
    [ -d "$ROOT/assets" ] || { echo "no assets/ to pack" >&2; exit 1; }
    mkdir -p "$ROOT/content"
    rm -f "$ZIP"
    (cd "$ROOT/assets" && zip -0 -r -q "$ZIP" . -x '.recomp-content-verified')
    {
      echo "# $TITLE game content - $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      echo "# archive"
      (cd "$ROOT/content" && sha256sum "$(basename "$ZIP")")
      echo "# files"
      (cd "$ROOT/assets" && find . -type f ! -name '.recomp-content-verified' \
        -exec sha256sum {} + | sort -k2)
    } > "$SUMS"
    echo "packed $(du -h "$ZIP" | cut -f1) -> $ZIP"
    echo "checksums -> $SUMS"
    ;;
  restore)
    [ -f "$ZIP" ] || { echo "no archive at $ZIP - copy it in first" >&2; exit 1; }
    if [ -f "$SUMS" ]; then
      echo "verifying archive"
      (cd "$ROOT/content" && grep -A1 '^# archive$' "$SUMS" | tail -1 | sha256sum -c -) || {
        echo "archive checksum does not match content.sha256" >&2; exit 1; }
    fi
    mkdir -p "$ROOT/assets"
    unzip -q -o "$ZIP" -d "$ROOT/assets"
    echo "restored into $ROOT/assets"
    "$0" verify
    ;;
  verify)
    [ -f "$SUMS" ] || { echo "no $SUMS to verify against" >&2; exit 1; }
    (cd "$ROOT/assets" && sed -n '/^# files$/,$p' "$SUMS" | tail -n +2 | sha256sum -c --quiet -) \
      && echo "game content verified"
    ;;
  *)
    echo "usage: $(basename "$0") pack|restore|verify" >&2
    exit 2
    ;;
esac
