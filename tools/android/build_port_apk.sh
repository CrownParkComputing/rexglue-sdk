#!/usr/bin/env bash
# Cross-compile a rexglue port for Android and package it as an APK.
#
#   build_port_apk.sh <port-dir> <slug> "<App Name>"
#
# Deliberately cds into the port itself: package_apk.sh takes $PWD-relative
# paths, and running it from anywhere else silently produces nothing while the
# previously installed APK carries on being the one that launches - which reads
# as "the fix had no effect".
set -euo pipefail

PORT="$(cd -- "$1" && pwd)"; SLUG="$2"; APP_NAME="$3"
SDK="${REXSDK_DIR:-/home/jon/rexglue-vmx}"
NDK="${ANDROID_NDK:-$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/* | sort -V | tail -1)}"

cd "$PORT"

# rexglue.cmake is emitted by codegen and needs the cross-compiling guards the
# SDK template now carries; regenerating is quicker than patching and cannot
# drift from the template.
echo "==> refreshing generated build files"
"$SDK/out/install/linux-amd64/bin/rexglue" codegen "${SLUG}_manifest.toml" 2>&1 | tail -1

echo "==> configure (android arm64)"
cmake -S . -B out/build/android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
    -DCMAKE_BUILD_TYPE=Release -DREXSDK_DIR="$SDK" >/dev/null

echo "==> build"
ninja -C out/build/android

mkdir -p out/android
echo "==> package"
"$SDK/tools/android/package_apk.sh" "$SLUG" "$APP_NAME" \
    "$PORT/out/build/android/libmain.so" "$SDK/out/android-arm64-v8a" \
    "$PORT/out/android/${SLUG}.apk"
