#!/usr/bin/env bash
# Make sure assets/ holds the right game content, importing the zip if it does
# not. Safe to run on every build: a correct tree costs one checksum pass.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if "$ROOT/tools/content_zip.sh" verify >/dev/null 2>&1; then
  echo "game content present and verified"
  exit 0
fi
echo "game content missing or changed - importing from content/"
"$ROOT/tools/content_zip.sh" restore
