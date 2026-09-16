#!/usr/bin/env bash
# Pack a built port as a Windows launcher bundle - a zip that runs, minus the
# game: the title's executable, the runtime and GPU DLLs, the MinGW runtime
# DLLs, rexiso.exe, its config, a PowerShell launcher and importer, and the
# facts/notes. No game data is included: the importer takes the player's own
# rip (archive, .iso or folder) into assets\ and checks it against
# content\content.sha256, as the Linux launcher does.
#
#   bundle_windows.sh <port-dir> <out.zip> [slug]
#
# Needs the port cross-built into out/build/windows (toolchain
# cmake/toolchains/windows-mingw-clang.cmake) and the SDK installed to
# out/install/win-amd64.
#
#   <slug>/<slug>.exe          the title           <slug>/librexruntime.dll   runtime
#   <slug>/rexgpu-xenos.dll    GPU layer           <slug>/rexiso.exe          disc images
#   <slug>/Launcher.bat        GUI                 <slug>/Play.bat            straight to play
#   <slug>/tools/*.ps1         importer + GUI      <slug>/content/            checksums, source
set -euo pipefail
PORT="$(cd -- "$1" && pwd)"
mkdir -p "$(dirname -- "$2")"
OUT="$(cd -- "$(dirname -- "$2")" && pwd)/$(basename -- "$2")"
SLUG="${3:-$(sed -n 's/^name = "\(.*\)"$/\1/p' "$PORT"/*_manifest.toml 2>/dev/null | head -1)}"
[ -n "$SLUG" ] || SLUG="$(basename "$PORT" | sed 's/-recomp$//')"
SDK="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL="$SDK/out/install/win-amd64/bin"
MINGW="${MINGW_BIN:-/usr/x86_64-w64-mingw32/bin}"
BUILD="$PORT/out/build/windows"
STRIP="$(command -v x86_64-w64-mingw32-strip || command -v llvm-strip || true)"

[ -f "$BUILD/$SLUG.exe" ] || { echo "no built executable at $BUILD/$SLUG.exe" >&2; exit 1; }
[ -f "$PORT/content/content.sha256" ] || { echo "no content/content.sha256 - pack the content once so the launcher can verify an import" >&2; exit 1; }

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/rexglue-winlauncher.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
D="$STAGE/$SLUG"
mkdir -p "$D/tools" "$D/config" "$D/content" "$D/user-data"

# --- code ----------------------------------------------------------------------
cp "$BUILD/$SLUG.exe" "$D/"
# Windows has no RPATH: everything the loader opens by name sits beside the exe,
# and the GPU plugin is looked up there as rexgpu-<name>.dll.
for dll in librexruntime.dll rexgpu-xenos.dll; do
  [ -f "$INSTALL/$dll" ] || { echo "missing $INSTALL/$dll - install the Windows SDK build first" >&2; exit 1; }
  cp "$INSTALL/$dll" "$D/"
done
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
  [ -f "$MINGW/$dll" ] || { echo "missing MinGW runtime $MINGW/$dll" >&2; exit 1; }
  cp "$MINGW/$dll" "$D/"
done
cp "$INSTALL/rexiso.exe" "$D/"
[ -n "$STRIP" ] && "$STRIP" "$D/$SLUG.exe" "$D/librexruntime.dll" "$D/rexgpu-xenos.dll" "$D/rexiso.exe" 2>/dev/null || true
cp "$PORT/config/$SLUG.toml" "$D/config/"
cp "$D/config/$SLUG.toml" "$D/$SLUG.toml"   # the runtime reads it beside the executable
if [ -d "$BUILD/shader_seed" ]; then cp -r "$BUILD/shader_seed" "$D/shader_seed"; fi
for m in "$BUILD"/lib${SLUG}_*.dll "$BUILD"/${SLUG}_*.dll; do [ -f "$m" ] && cp "$m" "$D/"; done

# --- tools: the player-facing set, CRLF because Notepad is where they get read --
crlf() { sed 's/$/\r/' "$1" > "$2"; }
crlf "$SDK/tools/windows/Import-GameFiles.ps1" "$D/tools/Import-GameFiles.ps1"
crlf "$SDK/tools/windows/Launcher.ps1" "$D/tools/Launcher.ps1"

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
  echo "platform=windows-x86_64"
  echo "functions=$FUNCS"
  echo "files=$FILES"
  echo "built=$(date -u +%Y-%m-%d)"
  echo "port_commit=$(git -C "$PORT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "sdk_commit=$(git -C "$SDK" rev-parse --short HEAD 2>/dev/null || echo unknown)"
} | sed 's/$/\r/' > "$D/bundle.txt"

# --- launchers ------------------------------------------------------------------
sed 's/$/\r/' > "$D/Launcher.bat" <<BAT
@echo off
rem ${NAME:-$SLUG} - launcher window: Import game files / Play / Close.
cd /d "%~dp0"
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tools\\Launcher.ps1"
BAT
sed 's/$/\r/' > "$D/Play.bat" <<BAT
@echo off
rem ${NAME:-$SLUG} - imports your game files on first run, then plays.
cd /d "%~dp0"
if not exist "assets\\default.xex" (
  powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tools\\Import-GameFiles.ps1"
  if not exist "assets\\default.xex" exit /b 1
)
start "" "$SLUG.exe" --game_data_root=assets --gpu_plugin xenos --user_data_root=user-data --license_mask=1 --mnk_mode %*
BAT
sed 's/$/\r/' > "$D/README.txt" <<TXT
${NAME:-$SLUG} - native Windows port (launcher)

  1. Launcher.bat             opens the launcher: Import game files / Play
     - or -   Play.bat        imports on first run, then plays
  2. Import your own copy of the game when asked. Expected source:
     $(head -1 "$PORT/content/SOURCE.txt" 2>/dev/null || echo "see content\SOURCE.txt")
     A .zip / .7z / .rar as downloaded, a .iso disc image, or the extracted
     folder all work; the files are copied into assets\ here and checked
     against content\content.sha256. 7-Zip is needed for .7z and .rar.
  3. Play. Saves and settings live in user-data\.

Needs: Windows 10/11 x64 with a Vulkan GPU driver (the renderer is Vulkan).
Keyboard: Return = Start, Space = A, WASD = stick, E = accelerate, or a controller.

No game data is included and nothing is downloaded. CONVERSION.md says what was
done to this title; NATIVE_COVERAGE.md how much of the console layer is native.
TXT

# --- pack -------------------------------------------------------------------------
rm -f "$OUT"
( cd "$STAGE" && zip -qr "$OUT" "$SLUG" )
echo "launcher: $OUT ($(( $(stat -c%s "$OUT") / 1048576 ))M)"
