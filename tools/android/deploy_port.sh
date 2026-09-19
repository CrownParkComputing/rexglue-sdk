#!/usr/bin/env bash
# Install a port's APK and push its game tree to the device.
#
#   deploy_port.sh <port-dir> <slug>
#
# THE APK IS CODE, THE TREE IS THE GAME. They are two separate deliveries and
# each is quiet about the other: a title that starts and reports no default.xex
# has had one and not the other.
set -euo pipefail

PORT="$(cd -- "$1" && pwd)"; SLUG="$2"
PKG="com.crownpark.rexglue.${SLUG}"
ADB="${ANDROID_HOME:-$HOME/Android/Sdk}/platform-tools/adb"
ADB_ARGS=()
[ -n "${ANDROID_SERIAL:-}" ] && ADB_ARGS=(-s "$ANDROID_SERIAL")

"$ADB" "${ADB_ARGS[@]}" install -r "$PORT/out/android/${SLUG}.apk"
"$ADB" "${ADB_ARGS[@]}" shell mkdir -p "/sdcard/Android/data/$PKG/files/game" "/sdcard/Android/data/$PKG/files/user"
echo "==> pushing game tree"
"$ADB" "${ADB_ARGS[@]}" push "$PORT/assets/." "/sdcard/Android/data/$PKG/files/game/" | tail -1
echo "==> $PKG ready; launch with:"
echo "    $ADB ${ADB_ARGS[*]} shell am start -n $PKG/.MainActivity"
