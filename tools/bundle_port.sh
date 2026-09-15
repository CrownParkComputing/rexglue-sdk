#!/usr/bin/env bash
# Pack a port into ONE standalone archive holding every platform we build:
# Windows, Linux and the Android APK, sharing a single copy of the game tree.
#
#   bundle_port.sh <port-dir> <slug> <out.zip>
#
# Unzip and run. The game tree is the bulk of a port and is identical on all
# three, so it is stored once here rather than once per platform archive - the
# combined bundle is smaller than any two of the separate ones were.
#
# Layout:
#   <slug>/play.sh      Linux launcher      <slug>/linux/    executable + .so
#   <slug>/play.bat     Windows launcher    <slug>/windows/  executable + .dll
#   <slug>/assets/      the game tree       <slug>/android/  APK + installer
set -euo pipefail

PORT="$(cd -- "$1" && pwd)"; SLUG="$2"
mkdir -p "$(dirname -- "$3")"
OUT="$(cd -- "$(dirname -- "$3")" && pwd)/$(basename -- "$3")"

SDK="${REXSDK_DIR:-/home/jon/rexglue-vmx}"
SDK_LINUX="$SDK/out/install/linux-amd64/lib"
SDK_WIN="$SDK/out/win-amd64"
MINGW="${MINGW_BIN:-/usr/x86_64-w64-mingw32/bin}"
PACKAGE="com.crownpark.rexglue.${SLUG}"

LIN_BIN="$PORT/out/build/linux/$SLUG"
WIN_BIN="$PORT/out/build/win-amd64/$SLUG.exe"
APK="$PORT/out/android/$SLUG.apk"

STAGE="$(mktemp -d "$(dirname "$OUT")/.bundle.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/$SLUG"
mkdir -p "$ROOT/assets" "$ROOT/user-data"

have_linux=0 have_windows=0 have_android=0

# --- the game tree, once ------------------------------------------------------
[ -d "$PORT/assets" ] || { echo "no game tree at $PORT/assets" >&2; exit 1; }
cp -a "$PORT/assets/." "$ROOT/assets/"

# The runtime reads <slug>.toml from the executable's own directory, so the
# per-title tuning has to be beside each binary, not in a shared config folder.
TOML="$PORT/config/$SLUG.toml"

# --- Linux --------------------------------------------------------------------
if [ -x "$LIN_BIN" ]; then
    mkdir -p "$ROOT/linux"
    cp "$LIN_BIN" "$ROOT/linux/$SLUG"
    strip "$ROOT/linux/$SLUG" 2>/dev/null || true
    for lib in librexruntime.so librexgpu-xenos.so; do
        [ -f "$SDK_LINUX/$lib" ] || continue
        cp "$SDK_LINUX/$lib" "$ROOT/linux/"
        strip "$ROOT/linux/$lib" 2>/dev/null || true
    done
    # The native GPU plugin is not part of the install tree (it is a research
    # backend, not a shipped one); take it from the build output when it exists
    # so --gpu_plugin native works on Linux too.
    if [ -f "$SDK/out/linux-amd64/Release/librexgpu-native.so" ]; then
        cp "$SDK/out/linux-amd64/Release/librexgpu-native.so" "$ROOT/linux/"
        strip "$ROOT/linux/librexgpu-native.so" 2>/dev/null || true
    fi
    [ -f "$TOML" ] && cp "$TOML" "$ROOT/linux/"
    have_linux=1
fi

# --- Windows ------------------------------------------------------------------
if [ -f "$WIN_BIN" ]; then
    mkdir -p "$ROOT/windows"
    cp "$WIN_BIN" "$ROOT/windows/$SLUG.exe"
    x86_64-w64-mingw32-strip "$ROOT/windows/$SLUG.exe" 2>/dev/null || true
    for dll in librexruntime.dll rexgpu-xenos.dll rexgpu-native.dll; do
        [ -f "$SDK_WIN/$dll" ] || continue
        cp "$SDK_WIN/$dll" "$ROOT/windows/"
        x86_64-w64-mingw32-strip "$ROOT/windows/$dll" 2>/dev/null || true
    done
    for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
        [ -f "$MINGW/$dll" ] || { echo "missing MinGW runtime $dll" >&2; exit 1; }
        cp "$MINGW/$dll" "$ROOT/windows/"
    done
    [ -f "$TOML" ] && cp "$TOML" "$ROOT/windows/"
    have_windows=1
fi

# --- Android ------------------------------------------------------------------
if [ -f "$APK" ]; then
    mkdir -p "$ROOT/android"
    cp "$APK" "$ROOT/android/$SLUG.apk"
    have_android=1
fi

[ $((have_linux + have_windows + have_android)) -gt 0 ] \
    || { echo "nothing built for $SLUG" >&2; exit 1; }

# --- launchers ----------------------------------------------------------------
crlf() { sed 's/$/\r/' > "$1"; }

if [ $have_linux -eq 1 ]; then
cat > "$ROOT/play.sh" <<LAUNCH
#!/usr/bin/env bash
# Linux launcher. Everything this needs is in this folder.
cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")"
exec env LD_LIBRARY_PATH="\$PWD/linux" ./linux/$SLUG \\
     --game_data_root=./assets --user_data_root=./user-data \\
     --gpu_plugin xenos --license_mask=1 --mnk_mode "\$@"
LAUNCH
chmod +x "$ROOT/play.sh"
fi

if [ $have_windows -eq 1 ]; then
crlf "$ROOT/play.bat" <<LAUNCH
@echo off
rem Windows launcher. Everything this needs is in this folder.
cd /d "%~dp0windows"
start "" "$SLUG.exe" --game_data_root=..\\assets --user_data_root=..\\user-data ^
      --gpu_plugin xenos --license_mask=1 --mnk_mode %*
LAUNCH
fi

if [ $have_android -eq 1 ]; then
cat > "$ROOT/android/install.sh" <<INSTALL
#!/usr/bin/env bash
# Install on an Android device over adb: the APK is code only, so the game
# tree has to be pushed separately - a title that installs and then reports no
# default.xex is one of the two halves having arrived.
set -euo pipefail
cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")"
adb install -r "$SLUG.apk"
adb shell mkdir -p "/sdcard/Android/data/$PACKAGE/files/game"
adb push ../assets/. "/sdcard/Android/data/$PACKAGE/files/game/"
echo "installed - launch $SLUG from the app drawer"
INSTALL
chmod +x "$ROOT/android/install.sh"

crlf "$ROOT/android/install.bat" <<INSTALL
@echo off
rem Same as install.sh, for a Windows PC with adb on PATH.
cd /d "%~dp0"
adb install -r "$SLUG.apk"
adb shell mkdir -p "/sdcard/Android/data/$PACKAGE/files/game"
adb push ..\\assets\\. "/sdcard/Android/data/$PACKAGE/files/game/"
echo installed - launch $SLUG from the app drawer
INSTALL
fi

# --- README -------------------------------------------------------------------
{
printf '%s - native port, all platforms in one\r\n\r\n' "$SLUG"
[ $have_windows -eq 1 ] && printf 'Windows (x86-64)   double-click play.bat\r\n'
[ $have_linux   -eq 1 ] && printf 'Linux (x86-64)     ./play.sh\r\n'
[ $have_android -eq 1 ] && printf 'Android (arm64)    android/install.sh  (or install.bat) with the device on adb\r\n'
printf '\r\n'
printf 'No installation on desktop. The executables, the runtime and GPU\r\n'
printf 'libraries, and the game tree in assets/ are all here. assets/ is shared\r\n'
printf 'by all three platforms, which is why this is one download instead of\r\n'
printf 'three - the Android APK carries code only and its installer pushes the\r\n'
printf 'same tree to the device.\r\n\r\n'
printf 'Saves and settings go in user-data/.\r\n\r\n'
printf 'Per-title tuning is %s.toml, kept beside each executable because that is\r\n' "$SLUG"
printf 'where the runtime reads it from. Edit both copies to change both.\r\n\r\n'
printf 'The fully native renderer can be tried with --gpu_plugin native passed\r\n'
printf 'to play.sh or play.bat. It is incomplete - some titles render black or\r\n'
printf 'fragmented - so xenos is the default because it is the one that draws\r\n'
printf 'correctly.\r\n\r\n'
printf 'Desktop needs a GPU with Vulkan 1.2 drivers.\r\n'
} > "$ROOT/README.txt"

( cd "$STAGE" && zip -qr "$OUT" "$SLUG" )
printf '  %-34s %6s MB   [%s]\n' "$(basename "$OUT")" \
    "$(( $(stat -c%s "$OUT") / 1048576 ))" \
    "$( { [ $have_windows -eq 1 ] && printf 'windows '; [ $have_linux -eq 1 ] && printf 'linux '; [ $have_android -eq 1 ] && printf 'android'; } )"
