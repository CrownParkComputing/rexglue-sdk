#!/usr/bin/env bash
# Pack a port as a LAUNCHER: everything except the game.
#
#   bundle_launcher.sh <port-dir> <out.tar.zst> [slug]
#
# This is the shape a port is published in. The executable, the two runtime
# libraries it loads, its configuration, the GUI and importer, the checksums
# of the game files it was built from and the conversion notes go in; the game
# tree does not - the person running it imports their own copy, which the
# checksums then verify. Unlike bundle_linux.sh (which deliberately includes
# the tree for a machine you own), nothing here is anyone's copyrighted data.
#
# Layout:
#   <slug>/<slug>            executable        <slug>/lib*.so         runtime
#   <slug>/run.sh            launcher          <slug>/tools/          GUI, importer
#   <slug>/config/           settings          <slug>/content/        SOURCE.txt, content.sha256
#   <slug>/CONVERSION.md     what was done     <slug>/NATIVE_COVERAGE.md
#   <slug>/bundle.txt        build facts       <slug>/README.txt
#   <slug>/shader_seed/      warmed pipelines  <slug>/lib<slug>_*.so  extra modules
set -euo pipefail
PORT="$(cd -- "$1" && pwd)"
OUT="$(cd -- "$(dirname -- "$2")" && pwd)/$(basename -- "$2")"
# The slug is the executable/config name: the manifest's project name, not the
# directory (rru-recomp builds ridgeracerunbounded). A third argument overrides.
SLUG="${3:-$(sed -n 's/^name = "\(.*\)"$/\1/p' "$PORT"/*_manifest.toml 2>/dev/null | head -1)}"
[ -n "$SLUG" ] || SLUG="$(basename "$PORT" | sed 's/-recomp$//')"
SDK="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$PORT/out/build/linux"
[ -x "$BUILD/$SLUG" ] || { echo "no built executable at $BUILD/$SLUG" >&2; exit 1; }
[ -f "$PORT/content/content.sha256" ] || { echo "no content/content.sha256 - pack the content once so the launcher can verify an import" >&2; exit 1; }

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/rexglue-launcher.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
D="$STAGE/$SLUG"
mkdir -p "$D/tools" "$D/config" "$D/content" "$D/user-data"

# --- code ----------------------------------------------------------------------
cp "$BUILD/$SLUG" "$D/"
for lib in librexruntime.so librexgpu-xenos.so; do
  src="$SDK/out/install/linux-amd64/lib/$lib"
  [ -f "$src" ] || src="$BUILD/$lib"
  cp "$src" "$D/$lib"
done
strip --strip-unneeded "$D/$SLUG" "$D"/lib*.so 2>/dev/null || true
cp "$PORT/config/$SLUG.toml" "$D/config/"
cp "$D/config/$SLUG.toml" "$D/$SLUG.toml"   # the runtime reads it beside the executable
# A port that ships a warmed shader/pipeline seed (shader_storage_seed_root)
# starts without compilation hitches; the directory is relative to the exe.
if [ -d "$BUILD/shader_seed" ]; then cp -r "$BUILD/shader_seed" "$D/shader_seed"; fi
# Multi-module titles: every recompiled module library lives beside the exe.
for m in "$BUILD"/lib${SLUG}_*.so; do [ -f "$m" ] && cp "$m" "$D/"; done

# --- tools: the player-facing set only -----------------------------------------
for t in port_gui.sh import_content.sh content_zip.sh port_info.py; do
  src="$PORT/tools/$t"; [ -f "$src" ] || src="$SDK/tools/$t"
  cp "$src" "$D/tools/$t"
done
cp "$SDK/tools/stfs_extract.py" "$D/tools/" 2>/dev/null || true
# rexiso lets the importer take a plain .iso (runtime XDVDFS reader, host tool).
REXISO="$SDK/out/install/linux-amd64/bin/rexiso"; [ -x "$REXISO" ] || REXISO="$SDK/out/linux-amd64/Release/rexiso"
cp "$REXISO" "$D/rexiso" && strip "$D/rexiso"
chmod +x "$D/tools/"*.sh

# --- facts and notes --------------------------------------------------------------
cp "$PORT/content/content.sha256" "$D/content/"
[ -f "$PORT/content/SOURCE.txt" ] && cp "$PORT/content/SOURCE.txt" "$D/content/"
[ -f "$PORT/CONVERSION.md" ] && cp "$PORT/CONVERSION.md" "$D/"
[ -f "$PORT/NATIVE_COVERAGE.md" ] && cp "$PORT/NATIVE_COVERAGE.md" "$D/"
FUNCS=$(cat "$PORT"/generated/default/*_recomp.*.cpp 2>/dev/null | grep -c "DEFINE_REX_FUNC(" || echo 0)
FILES=$(ls "$PORT"/generated/default/*_recomp.*.cpp 2>/dev/null | wc -l)
NAME="$(sed -n 's/^window_title = "\(.*\)"$/\1/p' "$PORT/config/$SLUG.toml" | head -1)"
{
  echo "name=${NAME:-$SLUG}"
  echo "slug=$SLUG"
  echo "functions=$FUNCS"
  echo "files=$FILES"
  echo "built=$(date -u +%Y-%m-%d)"
  echo "port_commit=$(git -C "$PORT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "sdk_commit=$(git -C "$SDK" rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$D/bundle.txt"

# --- launcher ---------------------------------------------------------------------
cat > "$D/run.sh" <<RUN
#!/usr/bin/env bash
# Launch ${NAME:-$SLUG}. The game files are not included: tools/port_gui.sh
# (or tools/import_content.sh) imports your own copy into assets/ first.
set -euo pipefail
ROOT="\$(cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")" && pwd)"
GAME="\${${SLUG^^}_GAME_DATA:-\$ROOT/assets}"
if ! "\$ROOT/tools/content_zip.sh" verify >/dev/null 2>&1 && [ ! -f "\$GAME/default.xex" ]; then
  "\$ROOT/tools/import_content.sh" || exit 1
fi
cd "\$ROOT"
exec env LD_LIBRARY_PATH="\$ROOT" ./$SLUG --game_data_root="\$GAME" --gpu_plugin xenos \\
  --user_data_root="\$ROOT/user-data" --license_mask=1 --mnk_mode "\$@"
RUN
chmod +x "$D/run.sh"

cat > "$D/README.txt" <<TXT
${NAME:-$SLUG} - native Linux port (launcher)

  1. tools/port_gui.sh        opens the launcher: Import game files / Play
     - or -   ./run.sh        imports on first run, then plays
  2. Import your own copy of the game when asked. Expected source:
     $(head -1 "$PORT/content/SOURCE.txt" 2>/dev/null || echo "see content/SOURCE.txt")
     A .iso of the disc, a .rar/.zip/.7z of its files, or the extracted folder
     all work; the files are copied into assets/ here and checked against
     content/content.sha256.
  3. Play. Saves and settings live in user-data/.

Needs: Linux x86-64, a Vulkan GPU driver, zenity (for the GUI), 7z or unrar for
archives, python3. Keyboard: Return = Start, Space = A, WASD = stick, E = accelerate,
or a game controller.

No game data is included and nothing is downloaded. CONVERSION.md says what was
done to this title; NATIVE_COVERAGE.md how much of the console layer is native.
TXT

# --- pack -------------------------------------------------------------------------
rm -f "$OUT"
tar --zstd -cf "$OUT" -C "$STAGE" "$SLUG"
echo "launcher: $OUT ($(du -h "$OUT" | cut -f1))"
tar --zstd -tf "$OUT" | sed 's#^#  #' | head -30
