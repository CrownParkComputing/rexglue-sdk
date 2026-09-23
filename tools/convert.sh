#!/usr/bin/env bash
# Convert a title and take it all the way through the pipeline.
#
#   convert.sh <archive-or-folder> <slug>
#
# Replaces the old convert_title.sh, which stopped at "it links" and called
# that done. It is not done: a port that has never been driven into gameplay
# and never had its imports traced is compiled, not converted. Those stages are
# mechanical, so they run here rather than waiting for somebody to remember
# them.
#
# Accepts what rips actually come as - a .rar/.zip/.7z or a folder, holding
# either a disc tree (default.xex) or an XBLA STFS package - because the
# package shape is not a game tree until stfs_extract.py has run.
#
# Two decisions are baked in, both because they cost real time to rediscover:
#   * setjmp/longjmp is declared BEFORE the first build. Undeclared, the longjmp
#     compiles to an ordinary call, control never unwinds, and the title renders
#     nothing for reasons that show up nowhere near the cause.
#   * non_volatile_as_local starts OFF. It is worth ~9% when it works, but it
#     corrupted every frame on Burnout Revenge; earn it back per title after the
#     port is known good.
set -uo pipefail

SRC="${1:?usage: convert.sh <archive-or-folder> <slug>}"
SLUG="${2:?usage: convert.sh <archive-or-folder> <slug>}"
SDK="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
REX="$SDK/out/install/linux-amd64/bin/rexglue"
RIP="$HOME/rips/$SLUG"
PROJ="${PROJECT_ROOT:-$HOME/recomp-ports/$SLUG-recomp}"

step() { printf '\n== %s\n' "$*"; }

# ------------------------------------------------------------- the rip -------
if [ ! -f "$RIP/default.xex" ]; then
  step "extracting $SLUG"
  WORK=""
  if [ -f "$SRC" ]; then
    WORK="$(mktemp -d "${TMPDIR:-/tmp}/convert.XXXXXX")"
    case "${SRC,,}" in
      *.rar) unrar x -o+ -idq "$SRC" "$WORK/" ;;
      *.zip) unzip -qq -o "$SRC" -d "$WORK" ;;
      *)     7z x -y -bso0 -bsp0 -o"$WORK" "$SRC" ;;
    esac
    TREE="$WORK"
  else
    TREE="$SRC"
  fi
  XEX="$(find "$TREE" -iname 'default.xex' -type f 2>/dev/null | head -1)"
  if [ -n "$XEX" ]; then
    mkdir -p "$RIP"; cp -a "$(dirname "$XEX")/." "$RIP/"
  else
    PKG="$(find "$TREE" -type f -size +1M 2>/dev/null | while read -r f; do
             case "$(head -c4 "$f" 2>/dev/null)" in LIVE|CON\ |PIRS) echo "$f"; break;; esac; done)"
    [ -n "$PKG" ] || { echo "no default.xex and no STFS package in $SRC" >&2; exit 1; }
    python3 "$SDK/tools/stfs_extract.py" extract "$PKG" "$RIP" >/dev/null || exit 1
  fi
  [ -n "$WORK" ] && rm -rf "$WORK"
fi
[ -f "$RIP/default.xex" ] || { echo "no default.xex in $RIP" >&2; exit 1; }
echo "   rip: $(du -sh "$RIP" | cut -f1)"

# ----------------------------------------------------------- the project -----
if [ ! -d "$PROJ" ]; then
  step "scaffolding $PROJ"
  "$SDK/tools/new_port.sh" --name "$SLUG" --content "$RIP" --project-root "$PROJ" >/dev/null || exit 1
fi
cd "$PROJ" || exit 1
MAN="$(ls ./*_manifest.toml 2>/dev/null | head -1)"
[ -n "$MAN" ] || { echo "no manifest in $PROJ" >&2; exit 1; }

sed -i 's/^non_volatile_as_local = true/non_volatile_as_local = false/' "$MAN"

if ! grep -q '^longjmp_address' "$MAN"; then
  step "finding setjmp/longjmp"
  OUT="$(python3 "$SDK/tools/find_setjmp.py" "$PROJ" 2>/dev/null)"
  LJ="$(sed -n 's/^longjmp_address = //p' <<<"$OUT" | head -1)"
  SJ="$(sed -n 's/^setjmp_address = //p' <<<"$OUT" | head -1)"
  if [ -n "$LJ" ] && [ -n "$SJ" ]; then
    python3 - "$MAN" "$LJ" "$SJ" <<'PY'
import re, sys
man, lj, sj = sys.argv[1:4]
s = open(man).read()
b = (f"\n# Found by tools/find_setjmp.py. Undeclared, the longjmp is emitted as an\n"
     f"# ordinary call: control never unwinds and the caller continues with\n"
     f"# non-volatile registers restored from a stale jmp_buf.\n"
     f"longjmp_address = {lj}\nsetjmp_address = {sj}")
s = re.sub(r'^(includes = \[\])$', lambda m: m.group(1) + b, s, count=1, flags=re.M)
open(man, 'w').write(s)
PY
    echo "   $LJ / $SJ"
  else
    echo "   none found (not every title uses them)"
  fi
fi

# Copy the current tooling in, so a port converted today has the checks that
# were learned yesterday.
cp "$SDK/tools/"{pipeline.py,port_info.py,native_report.py,verify_port.sh,port_gui.sh,import_content.sh} tools/ 2>/dev/null
chmod +x tools/*.sh tools/*.py 2>/dev/null
sed -i "s/^window_title = \"\"/window_title = \"${SLUG}\"/" "config/$SLUG.toml" 2>/dev/null

# ------------------------------------------------------------ the stages -----
step "pipeline: codegen -> build -> verify -> trace -> native"
python3 tools/pipeline.py "$PROJ" all
RC=$?

echo
python3 tools/pipeline.py "$PROJ" status
exit $RC
