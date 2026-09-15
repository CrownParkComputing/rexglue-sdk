#!/usr/bin/env bash
# Pack a built port into one self-contained Windows folder: the executable, the
# SDK DLLs it loads, the MinGW runtime DLLs, its config, its game tree and a
# launcher, zipped.
#
#   bundle_windows.sh <port-dir> <slug> <out.zip>
#
# Like bundle_linux.sh this deliberately includes the game tree: one file that
# runs, rather than a code drop that starts and reports no default.xex.
set -euo pipefail

PORT="$(cd -- "$1" && pwd)"; SLUG="$2"
mkdir -p "$(dirname -- "$3")"
OUT="$(cd -- "$(dirname -- "$3")" && pwd)/$(basename -- "$3")"
SDK="${REXSDK_DIR:-/home/jon/rexglue-vmx}/out/win-amd64"
MINGW="${MINGW_BIN:-/usr/x86_64-w64-mingw32/bin}"
BIN="$PORT/out/build/win-amd64/$SLUG.exe"
[ -x "$BIN" ] || { echo "no built executable at $BIN" >&2; exit 1; }

STAGE="$(mktemp -d "$(dirname "$OUT")/.bundle.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/$SLUG"
mkdir -p "$ROOT/assets" "$ROOT/user-data"

cp "$BIN" "$ROOT/$SLUG.exe"
x86_64-w64-mingw32-strip "$ROOT/$SLUG.exe" 2>/dev/null || true

# Everything the loader opens by name sits beside the executable: Windows has
# no RPATH, and the GPU plugin is looked up as rexgpu-<name>.dll in the
# executable's own directory.
for dll in librexruntime.dll rexgpu-xenos.dll rexgpu-native.dll; do
    [ -f "$SDK/$dll" ] || continue
    cp "$SDK/$dll" "$ROOT/"
    x86_64-w64-mingw32-strip "$ROOT/$dll" 2>/dev/null || true
done
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    [ -f "$MINGW/$dll" ] || { echo "missing MinGW runtime $dll" >&2; exit 1; }
    cp "$MINGW/$dll" "$ROOT/"
done

[ -d "$PORT/config" ] && cp -r "$PORT/config" "$ROOT/" || true
cp -a "$PORT/assets/." "$ROOT/assets/"

# CRLF: this is read in Notepad on the target machine.
mk_crlf() { sed 's/$/\r/' > "$1"; }

mk_crlf "$ROOT/play.bat" <<LAUNCH
@echo off
rem Self-contained launcher: everything this needs is in this folder.
cd /d "%~dp0"
start "" "$SLUG.exe" --game_data_root=.\\assets --gpu_plugin xenos ^
     --user_data_root=.\\user-data --license_mask=1 %*
LAUNCH

mk_crlf "$ROOT/README.txt" <<INFO
$SLUG - native Windows build (x86-64)

    play.bat

Everything needed is in this folder: the executable, the runtime and GPU DLLs,
and the game tree in assets\\. No installation.

The fully native renderer can be tried with:
    play.bat --gpu_plugin native
It is incomplete - some titles render black or fragmented - so xenos is the
default because it is the one that draws correctly.

Requires a GPU with Vulkan 1.2 drivers.
INFO

( cd "$STAGE" && zip -qr "$OUT" "$SLUG" )
echo "  $(basename "$OUT")  $(( $(stat -c%s "$OUT") / 1048576 )) MB"
