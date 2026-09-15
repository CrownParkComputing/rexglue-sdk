#!/usr/bin/env bash
# Pack a built port into one self-contained Linux archive: the executable, the
# SDK libraries it loads, its config, its game tree and a launcher.
#
#   bundle_linux.sh <port-dir> <slug> <out.tar.zst>
#
# Deliberately includes the game tree. The Android APK carries code only and
# the tree is a separate delivery, which is a genuine trap - a title installs,
# starts, and reports no default.xex because one of the two arrived. A desktop
# bundle has no such excuse: it is one file that runs.
set -euo pipefail

PORT="$(cd -- "$1" && pwd)"; SLUG="$2"; OUT="$3"
SDK="${REXSDK_DIR:-/home/jon/rexglue-vmx}/out/install/linux-amd64/lib"
BIN="$PORT/out/build/linux/$SLUG"
[ -x "$BIN" ] || { echo "no built executable at $BIN" >&2; exit 1; }

STAGE="$(mktemp -d "$(dirname "$OUT")/.bundle.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/$SLUG"
mkdir -p "$ROOT/lib" "$ROOT/assets"

cp "$BIN" "$ROOT/$SLUG"
strip "$ROOT/$SLUG" 2>/dev/null || true
for lib in librexruntime.so librexgpu-xenos.so librexgpu-native.so; do
    [ -f "$SDK/$lib" ] && cp "$SDK/$lib" "$ROOT/lib/" && strip "$ROOT/lib/$lib" 2>/dev/null || true
done
[ -d "$PORT/config" ] && cp -r "$PORT/config" "$ROOT/" || true
cp -a "$PORT/assets/." "$ROOT/assets/"

cat > "$ROOT/play.sh" <<LAUNCH
#!/usr/bin/env bash
# Self-contained launcher: everything this needs is in this directory.
cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")"
# The GPU plugin is looked for beside the executable, so lib/ is copied up
# rather than only added to the search path.
cp -n lib/*.so . 2>/dev/null || true
exec env LD_LIBRARY_PATH=. ./$SLUG --game_data_root=./assets --gpu_plugin xenos \\
     --user_data_root=./user-data --license_mask=1 "\$@"
LAUNCH
chmod +x "$ROOT/play.sh"
mkdir -p "$ROOT/user-data"

cat > "$ROOT/README.txt" <<INFO
$SLUG - native Linux build (x86-64)

    ./play.sh

Everything needed is in this directory: the executable, the runtime and GPU
libraries, and the game tree in assets/. No installation.

The fully native renderer can be tried with:
    ./play.sh --gpu_plugin native
It is incomplete - some titles render black or fragmented - so xenos is the
default because it is the one that draws correctly.
INFO

tar --zstd -cf "$OUT" -C "$STAGE" "$SLUG"
echo "  $(basename "$OUT")  $(( $(stat -c%s "$OUT") / 1048576 )) MB"
