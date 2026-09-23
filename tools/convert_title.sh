#!/usr/bin/env bash
# rar -> playable port, with everything this estate has learned already applied.
#
#   convert_title.sh "<rar>" <slug>
#
# The setjmp/longjmp step is the one that is easy to forget and expensive to
# miss: undeclared, the longjmp compiles to an ordinary call, control never
# unwinds, and the title renders nothing for reasons that show up nowhere near
# the cause. Both Geometry Wars titles were black for exactly that.
set -euo pipefail
RAR="$1"; SLUG="$2"
SDK=/home/jon/rexglue-vmx
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
RIP="$HOME/rips/$SLUG"
PROJ="$HOME/recomp-ports/$SLUG-recomp"

if [ ! -d "$RIP" ]; then
  unrar x -o+ -idq "$RAR" "$WORK/" 
  PKG="$(find "$WORK" -type f -size +1M | while read -r f; do
           case "$(head -c4 "$f")" in LIVE|CON\ |PIRS) echo "$f"; break;; esac; done)"
  [ -n "$PKG" ] || { echo "$SLUG: no STFS package in $RAR" >&2; exit 1; }
  python3 "$SDK/tools/stfs_extract.py" extract "$PKG" "$RIP" >/dev/null
fi
[ -f "$RIP/default.xex" ] || { echo "$SLUG: no default.xex after extract" >&2; exit 1; }

[ -d "$PROJ" ] || "$SDK/tools/new_port.sh" --name "$SLUG" --content "$RIP" --project-root "$PROJ" >/dev/null

# Declare setjmp/longjmp before the first real build, so the generated code is
# right the first time rather than after a black-screen investigation.
MAN="$(ls "$PROJ"/*_manifest.toml | head -1)"
if ! grep -q '^longjmp_address' "$MAN"; then
  if OUT=$(python3 "$SDK/tools/find_setjmp.py" "$PROJ" 2>/dev/null); then
    LJ=$(sed -n 's/^longjmp_address = //p' <<<"$OUT" | head -1)
    SJ=$(sed -n 's/^setjmp_address = //p' <<<"$OUT" | head -1)
    if [ -n "$LJ" ] && [ -n "$SJ" ]; then
      python3 - "$MAN" "$LJ" "$SJ" <<'PY'
import re, sys
man, lj, sj = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(man).read()
block = (f"\n# Found by tools/find_setjmp.py. Undeclared, the longjmp is emitted as an\n"
         f"# ordinary call: control never unwinds and the caller continues with\n"
         f"# non-volatile registers restored from a stale jmp_buf.\n"
         f"longjmp_address = {lj}\nsetjmp_address = {sj}")
s = re.sub(r'^(includes = \[\])$', lambda m: m.group(1) + block, s, count=1, flags=re.M)
open(man, 'w').write(s)
PY
      echo "  setjmp/longjmp: $LJ / $SJ - regenerating"
      ( cd "$PROJ" && "$SDK/out/install/linux-amd64/bin/rexglue" codegen "$(basename "$MAN")" >/dev/null 2>&1 )
    fi
  fi
fi

cmake -S "$PROJ" -B "$PROJ/out/build/linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ >/dev/null
cmake --build "$PROJ/out/build/linux" -j"$(nproc)" >/dev/null
echo "  $SLUG: built"
