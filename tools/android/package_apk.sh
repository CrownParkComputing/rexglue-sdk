#!/usr/bin/env bash
# Package a built rexglue port as a debug-signed APK. No Gradle, no network.
#
#   package_apk.sh <slug> <"App Name"> <libmain.so> <sdk-android-lib-dir> <out.apk>
#
# The APK is CODE only. The game tree is pushed separately to the app's own
# external files directory - see deploy_apk.sh. A title that starts and then
# says it has no default.xex has had one delivery and not the other.
set -euo pipefail

SLUG="$1"; APP_NAME="$2"; LIBMAIN="$3"; SDK_LIBS="$4"; OUT_APK="$5"
PACKAGE="com.crownpark.rexglue.${SLUG}"

SDK_ROOT="${ANDROID_HOME:-$HOME/Android/Sdk}"
BUILD_TOOLS="$(ls -d "$SDK_ROOT"/build-tools/* | sort -V | tail -1)"
PLATFORM_JAR="$(ls -d "$SDK_ROOT"/platforms/android-* | sort -V | tail -1)/android.jar"
SDL_JAVA="${REXSDK_DIR:-/home/jon/rexglue-vmx}/thirdparty/sdl3/android-project/app/src/main/java"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/lib/arm64-v8a" "$WORK/classes" "$WORK/res"

# The activity is SDL's own. It dlopens the libraries getLibraries() names, so
# the subclass below lists ours: SDL is linked statically into libmain, so only
# main is named, and the runtime/gpu libraries it NEEDs are resolved by the
# dynamic linker from the same directory.
mkdir -p "$WORK/src/${PACKAGE//./\/}"
cat > "$WORK/src/${PACKAGE//./\/}/MainActivity.java" <<JAVA
package ${PACKAGE};
import org.libsdl.app.SDLActivity;
public class MainActivity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] { "main" };
    }
    // There is no command line on Android, and the runtime needs to be told
    // where its content lives. The app's own external files directory needs no
    // storage permission and is removed when the app is uninstalled, so the
    // game tree is pushed to files/game and the profile written to files/user.
    @Override protected String[] getArguments() {
        String files = getExternalFilesDir(null).getAbsolutePath();
        return new String[] {
            "--game_data_root=" + files + "/game",
            "--user_data_root=" + files + "/user",
            // Logging starts before SDL does, so the runtime cannot ask Android
            // where it may write and falls back to the working directory -
            // which is /system/bin and read-only. Naming the file here is what
            // stops it aborting before it has logged anything at all.
            "--log_file=" + files + "/rexglue.log",
            // Without this no GPU emulation is loaded at all and every Vd*
            // call is ignored: the title runs, draws nothing, and says so only
            // in warnings.
            "--gpu_plugin", "xenos",
            "--license_mask=1",
        };
    }
}
JAVA

cat > "$WORK/AndroidManifest.xml" <<MANIFEST
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="${PACKAGE}" android:versionCode="1" android:versionName="0.1">
    <uses-sdk android:minSdkVersion="30" android:targetSdkVersion="34" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.hardware.gamepad" android:required="false" />
    <application android:label="${APP_NAME}" android:hasCode="true"
                 android:allowBackup="false"
                 android:extractNativeLibs="true"
                 android:debuggable="true">
        <activity android:name=".MainActivity"
                  android:exported="true"
                  android:configChanges="keyboard|keyboardHidden|orientation|screenSize|screenLayout|uiMode"
                  android:launchMode="singleInstance"
                  android:screenOrientation="landscape">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
MANIFEST

echo "==> java -> dex"
find "$SDL_JAVA" -name '*.java' > "$WORK/sources.txt"
find "$WORK/src" -name '*.java' >> "$WORK/sources.txt"
# --release rather than -source/-target/-bootclasspath: javac refuses the
# combination outright. And NOT piped through grep - a pipeline hides javac's
# exit status, which produced a signed APK with no classes.dex in it: an
# installable app that dies the moment it starts.
javac -nowarn --release 17 -classpath "$PLATFORM_JAR" \
      -d "$WORK/classes" @"$WORK/sources.txt"
"$BUILD_TOOLS/d8" --min-api 30 --output "$WORK" $(find "$WORK/classes" -name '*.class')
[ -f "$WORK/classes.dex" ] || { echo "d8 produced no classes.dex" >&2; exit 1; }

echo "==> native libraries"
# Stripped: the debug symbols are several hundred MB and the device does not
# need them. The unstripped copies stay in the build tree for symbolising.
STRIP="$(ls "$SDK_ROOT"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip | sort -V | tail -1)"
for so in "$LIBMAIN" "$SDK_LIBS"/librexruntime.so "$SDK_LIBS"/librexgpu-xenos.so; do
    cp "$so" "$WORK/lib/arm64-v8a/"
    "$STRIP" --strip-unneeded "$WORK/lib/arm64-v8a/$(basename "$so")"
done

echo "==> aapt2 link"
"$BUILD_TOOLS/aapt2" link -o "$WORK/base.apk" -I "$PLATFORM_JAR" \
    --manifest "$WORK/AndroidManifest.xml" --min-sdk-version 30 --target-sdk-version 34

echo "==> assembling"
cd "$WORK"
zip -q -r base.apk classes.dex lib
"$BUILD_TOOLS/zipalign" -f -p 4 base.apk aligned.apk

KEYSTORE="$HOME/.android/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$HOME/.android"
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
    --key-pass pass:android --ks-key-alias androiddebugkey --out "$OUT_APK" aligned.apk

# Prove the APK actually contains what it must. A missing dex or library
# installs perfectly happily and then fails on launch.
for required in classes.dex lib/arm64-v8a/libmain.so lib/arm64-v8a/librexruntime.so \
                lib/arm64-v8a/librexgpu-xenos.so; do
    unzip -l "$OUT_APK" | grep -q " $required\$" \
        || { echo "APK is missing $required" >&2; exit 1; }
done

echo "==> $OUT_APK"
ls -la "$OUT_APK"
echo "    package ${PACKAGE}"
