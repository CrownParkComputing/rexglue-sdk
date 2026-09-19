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
# Keep the ABI generic enough for every arm64-v8a device and optimise the very
# large generated translation units. Do not apply hidden visibility or LTO to
# the whole graph: librexruntime and librexgpu are separate DSOs and exchange
# C++ symbols. Global LTO/visibility internalises that API and produces plugins
# with unresolved runtime symbols. Section GC/ICF retains the safe size and
# startup wins without changing the DSO boundary.
ANDROID_RELEASE_CXX="-O3 -DNDEBUG -ffunction-sections -fdata-sections"
ANDROID_RELEASE_C="-O3 -DNDEBUG -ffunction-sections -fdata-sections"
ANDROID_RELEASE_LINK="-Wl,--gc-sections -Wl,--icf=safe -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384"
cmake -S . -B out/build/android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DREXSDK_DIR="$SDK" \
    -DCMAKE_C_FLAGS_RELEASE="$ANDROID_RELEASE_C" \
    -DCMAKE_CXX_FLAGS_RELEASE="$ANDROID_RELEASE_CXX" \
    -DCMAKE_SHARED_LINKER_FLAGS_RELEASE="$ANDROID_RELEASE_LINK" >/dev/null

echo "==> build"
ninja -C out/build/android

mkdir -p out/android
echo "==> package"
ANDROID_ARGS="${EXTRA_ARGS:-}"
# Android performance profile: keep the guest's supported 720p mode but use a
# smaller presentation surface on handhelds. Set ANDROID_WINDOW_WIDTH/HEIGHT
# to 1280/720 for the quality profile when packaging a larger device.
ANDROID_WINDOW_WIDTH="${ANDROID_WINDOW_WIDTH:-640}"
ANDROID_WINDOW_HEIGHT="${ANDROID_WINDOW_HEIGHT:-360}"
ANDROID_ARGS="--window_width=${ANDROID_WINDOW_WIDTH} --window_height=${ANDROID_WINDOW_HEIGHT} --resolution=640x360 ${ANDROID_ARGS}"
if [ -f "$PORT/config/android.args" ]; then
    while IFS= read -r arg; do
        case "$arg" in ''|'#'*) continue ;; esac
        ANDROID_ARGS="${ANDROID_ARGS:+$ANDROID_ARGS }$arg"
    done < "$PORT/config/android.args"
fi
EXTRA_ARGS="$ANDROID_ARGS" "$SDK/tools/android/package_apk.sh" "$SLUG" "$APP_NAME" \
    "$PORT/out/build/android/libmain.so" "$SDK/out/android-arm64-v8a" \
    "$PORT/out/android/${SLUG}.apk"
