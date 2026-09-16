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
# Per-title cvars. A desktop build reads config/<slug>.toml from beside the
# executable; on Android there is no such place, so anything the title needs
# has to be handed to it here. Space-separated, e.g.
#   EXTRA_ARGS="--clear_memory_page_state=true --render_target_path_vulkan=fsi"
EXTRA_ARGS="${EXTRA_ARGS:-}"
# Which GPU backend(s) to ship. "xenos" is full Xenos emulation and is what
# renders correctly today; "native" translates to real GPU commands with no
# EDRAM, tiling or resolve emulation. Space-separated to ship both and choose
# at runtime with --gpu_plugin.
GPU_PLUGINS="${GPU_PLUGINS:-xenos}"
GPU_PLUGIN_DEFAULT="${GPU_PLUGINS%% *}"   # the activity asks for the first one
PACKAGE="com.crownpark.rexglue.${SLUG}"

SDK_ROOT="${ANDROID_HOME:-$HOME/Android/Sdk}"
BUILD_TOOLS="$(ls -d "$SDK_ROOT"/build-tools/* | sort -V | tail -1)"
PLATFORM_JAR="$(ls -d "$SDK_ROOT"/platforms/android-* | sort -V | tail -1)/android.jar"
SDL_JAVA="${REXSDK_DIR:-/home/jon/rexglue-vmx}/thirdparty/sdl3/android-project/app/src/main/java"

# Staged beside the output APK, not in /tmp: the unstripped libraries are a
# few hundred MB and /tmp here is a 16 GB tmpfs shared with everything else.
# Filling it makes the NDK's clang report "IO failure on output stream", which
# reads as a compiler fault and is not one.
WORK="$(mktemp -d "$(dirname "$OUT_APK")/.package.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/lib/arm64-v8a" "$WORK/classes" "$WORK/res"

# The activity is SDL's own. It dlopens the libraries getLibraries() names, so
# the subclass below lists ours: SDL is linked statically into libmain, so only
# main is named, and the runtime/gpu libraries it NEEDs are resolved by the
# dynamic linker from the same directory.
mkdir -p "$WORK/src/${PACKAGE//./\/}"
EXTRA_JAVA=""
for a in $EXTRA_ARGS; do
    EXTRA_JAVA="${EXTRA_JAVA}        args.add(\"${a}\");
"
done
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
        java.util.ArrayList<String> args = new java.util.ArrayList<>(java.util.Arrays.asList(
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
            "--gpu_plugin", "${GPU_PLUGIN_DEFAULT}",
            "--license_mask=1"
        ));
${EXTRA_JAVA}
        // Anything in files/rexglue.args (one per line, # for comments) is
        // appended, so a setting can be changed on the device instead of
        // rebuilding and reinstalling to try one flag. Later arguments win.
        try {
            java.io.File extra = new java.io.File(files, "rexglue.args");
            if (extra.isFile()) {
                java.io.BufferedReader r =
                    new java.io.BufferedReader(new java.io.FileReader(extra));
                String line;
                while ((line = r.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("#")) { args.add(line); }
                }
                r.close();
            }
        } catch (java.io.IOException e) {
            android.util.Log.e("rexglue", "could not read rexglue.args", e);
        }
        return args.toArray(new String[0]);
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
        <!-- The title bar belongs to the ACTIVITY, not to SDL. The fullscreen
             cvar only affects the SDL window, so without this theme the app
             draws under a bar showing the window title and build string.
             (An XML comment may not contain a double hyphen, so cvars are
             named without their leading dashes here.) -->
        <activity android:name=".MainActivity"
                  android:theme="@android:style/Theme.NoTitleBar.Fullscreen"
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
PLUGIN_SOS=""
for plugin in $GPU_PLUGINS; do
    PLUGIN_SOS="$PLUGIN_SOS $SDK_LIBS/librexgpu-${plugin}.so"
done
for so in "$LIBMAIN" "$SDK_LIBS"/librexruntime.so $PLUGIN_SOS; do
    cp "$so" "$WORK/lib/arm64-v8a/"
    "$STRIP" --strip-unneeded "$WORK/lib/arm64-v8a/$(basename "$so")"
done
# Multi-module titles (Split/Second: launcher + SKIPPER + SPLITSECOND1) keep
# every recompiled module in its own lib<slug>_<MODULE>.so beside libmain.so.
# They ride along, and the runtime asks the linker for them by name on Android.
for so in "$(dirname "$LIBMAIN")"/lib${SLUG}_*.so; do
    [ -f "$so" ] || continue
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
REQUIRED="classes.dex lib/arm64-v8a/libmain.so lib/arm64-v8a/librexruntime.so"
for plugin in $GPU_PLUGINS; do
    REQUIRED="$REQUIRED lib/arm64-v8a/librexgpu-${plugin}.so"
done
for required in $REQUIRED; do
    unzip -l "$OUT_APK" | grep -q " $required\$" \
        || { echo "APK is missing $required" >&2; exit 1; }
done

echo "==> $OUT_APK"
ls -la "$OUT_APK"
echo "    package ${PACKAGE}"
