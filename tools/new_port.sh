#!/usr/bin/env bash
# Stamp out a new recomp project with everything we have learned already wired
# in: the per-title config profile, content packaging, headless testing and
# per-frame measurement, and a run script that cannot pick up stale SDK
# libraries.
#
#   tools/new_port.sh --name burnoutrevenge --content ~/rips/BurnoutRevenge
#
#   --name         short name; becomes the executable and the project folder
#   --content      directory holding the extracted disc (must contain the XEX)
#   --project-root where to create it (default ~/<name>-recomp)
#   --move         move the content instead of copying it (saves a disc-sized copy)
#   --no-pack      skip building the content zip (do it later with content_zip.sh pack)
#
# Deliberately stops before configure and build, and prints the two commands:
# code generation on a big title is minutes of CPU and should be a decision,
# not a side effect of asking for a folder.
set -euo pipefail

SDK="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
REXGLUE="${REXGLUE:-$SDK/out/install/linux-amd64/bin/rexglue}"
NAME=""; CONTENT=""; PROJECT_ROOT=""; MOVE=0; PACK=1

while [ $# -gt 0 ]; do
  case "$1" in
    --name) NAME="$2"; shift 2 ;;
    --content) CONTENT="$(cd -- "$2" && pwd)"; shift 2 ;;
    --project-root) PROJECT_ROOT="$2"; shift 2 ;;
    --move) MOVE=1; shift ;;
    --no-pack) PACK=0; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[ -n "$NAME" ] || { echo "--name is required" >&2; exit 2; }
[ -n "$CONTENT" ] || { echo "--content is required" >&2; exit 2; }
[ -x "$REXGLUE" ] || { echo "rexglue not found at $REXGLUE (build the SDK, or set REXGLUE)" >&2; exit 2; }
PROJECT_ROOT="${PROJECT_ROOT:-$HOME/$NAME-recomp}"
[ -e "$PROJECT_ROOT" ] && { echo "$PROJECT_ROOT already exists" >&2; exit 2; }

# The XEX is the entry point and the thing the scaffold keys off; a rip without
# one is a disc image that still needs extracting, not a game tree.
XEX="$(find "$CONTENT" -maxdepth 1 -iname 'default.xex' | head -1)"
[ -n "$XEX" ] || { echo "no default.xex in $CONTENT - is this an extracted disc tree?" >&2; exit 2; }

echo "==> creating $PROJECT_ROOT"
mkdir -p "$PROJECT_ROOT/assets" "$PROJECT_ROOT/config" "$PROJECT_ROOT/tools" "$PROJECT_ROOT/user-data"
if [ "$MOVE" = 1 ]; then
  mv "$CONTENT"/* "$PROJECT_ROOT/assets/"
else
  cp -a "$CONTENT"/. "$PROJECT_ROOT/assets/"
fi
# rexglue init insists the XEX lives under the game root, which is now assets/.
XEX_NAME="$(basename "$(find "$PROJECT_ROOT/assets" -maxdepth 1 -iname 'default.xex' | head -1)")"

echo "==> rexglue init"
(cd "$PROJECT_ROOT" && "$REXGLUE" init --project-name "$NAME" \
  --xex-path "$PROJECT_ROOT/assets/$XEX_NAME" --game-root "$PROJECT_ROOT/assets" \
  --project-root "$PROJECT_ROOT" >/dev/null)
# Keep the manifest relative so the project moves without editing.
sed -i "s|$PROJECT_ROOT/assets|assets|g" "$PROJECT_ROOT/${NAME}_manifest.toml"

echo "==> config, tooling and scripts"
cp "$SDK/tools/port_profile.toml" "$PROJECT_ROOT/config/$NAME.toml"
cp "$SDK/tools/content_zip.sh" "$SDK/tools/content_ensure.sh" "$PROJECT_ROOT/tools/"
for script in headless_play measure; do
  sed "s|@TITLE@|$NAME|g" "$SDK/tools/$script.sh.in" > "$PROJECT_ROOT/tools/$script.sh"
  chmod +x "$PROJECT_ROOT/tools/$script.sh"
done

cat > "$PROJECT_ROOT/run.sh" <<RUN
#!/usr/bin/env bash
# Run the recompiled title against the content in assets/.
set -euo pipefail
ROOT="\$(cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")" && pwd)"
BUILD="\$ROOT/out/build/linux"
GAME="\${${NAME^^}_GAME_DATA:-\$ROOT/assets}"
SDK_LIB="$SDK/out/install/linux-amd64/lib"

if [ ! -f "\$GAME/$XEX_NAME" ]; then
  echo "game content missing; importing content/*.zip" >&2
  "\$ROOT/tools/content_zip.sh" restore
fi
[ -x "\$BUILD/$NAME" ] || { echo "build first: cmake --build out/build/linux" >&2; exit 1; }

# The SDK libraries sit next to the executable and are NOT refreshed by the
# project build: a rebuilt SDK with no copy here runs the old code and every
# diagnostic you just added is silently missing. Sync whatever is newer.
for lib in librexruntime.so librexgpu-xenos.so; do
  [ "\$SDK_LIB/\$lib" -nt "\$BUILD/\$lib" ] && cp "\$SDK_LIB/\$lib" "\$BUILD/" || true
done
cp "\$ROOT/config/$NAME.toml" "\$BUILD/" 2>/dev/null || true

cd "\$BUILD"
exec env LD_LIBRARY_PATH=. ./$NAME --game_data_root="\$GAME" --gpu_plugin xenos \\
  --user_data_root="\$ROOT/user-data" --license_mask=1 --mnk_mode "\$@"
RUN
chmod +x "$PROJECT_ROOT/run.sh"

cat > "$PROJECT_ROOT/.gitignore" <<'IGNORE'
out/
user-data/
generated/
# The disc content is never committed: assets/ is imported from the zip under
# content/, and content.sha256 proves a restored tree is the right content.
assets/
content/*.zip
IGNORE

python3 - "$PROJECT_ROOT" "$NAME" <<'PATCH'
import sys
from pathlib import Path
root, name = Path(sys.argv[1]), sys.argv[2]
path = root / 'CMakeLists.txt'
text = path.read_text()
anchor = 'include(generated/rexglue.cmake)'
block = f'''# The disc content is not in Git. It lives as one zip under content/, imported
# into assets/ before code generation so a rebuild is a single step, with
# content/content.sha256 - which IS in Git - recording the archive checksum and
# every file's. Already-correct assets cost one checksum pass.
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_custom_target({name}_content
    COMMAND ${{CMAKE_COMMAND}} -E env bash
        ${{CMAKE_CURRENT_SOURCE_DIR}}/tools/content_ensure.sh
    COMMENT "Checking game content (importing content/*.zip if needed)"
    VERBATIM)

{anchor}

add_dependencies({name}_codegen {name}_content)'''
path.write_text(text.replace(anchor, block, 1))
PATCH

cat > "$PROJECT_ROOT/README.md" <<README
# $NAME

Recompiled Xbox 360 title on the ReXGlue SDK at \`$SDK\`.

## Build

Configure with **clang**. With g++ the \`DEFINE_REX_FUNC\` weak alias
(\`sub_X\` -> \`__imp__sub_X\`) is not emitted and the link fails with tens of
thousands of undefined \`sub_XXXXXXXX\` references from the register table.

\`\`\`sh
cmake -S . -B out/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \\
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++
cmake --build out/build/linux -j\$(nproc)
./run.sh
\`\`\`

## Content

\`assets/\` holds the disc files and is not in Git. \`tools/content_zip.sh\`
packs it into one zip under \`content/\` and imports it back, with
\`content/content.sha256\` (committed) recording the archive checksum and every
file's; the build imports automatically when assets/ is missing or wrong.

## Testing without a display

\`\`\`sh
tools/headless_play.sh 150 "25:Return 100:e:35"   # capture frames, drive menus
tools/measure.sh 150 "25:Return"                  # per-frame cost breakdown
tools/measure.sh 150 "25:Return" --gpu_hot_page_frames=0   # A/B a setting
\`\`\`

## Configuration

\`config/$NAME.toml\` starts from the shared profile in the SDK
(\`tools/port_profile.toml\`) - every setting there carries the measurement that
justifies it. If something renders wrong, turn them off in this order:
\`gpu_hot_page_frames\`, \`clear_memory_page_state\`, \`render_target_path_vulkan\`.
README

# Code generation must happen BEFORE the first cmake configure: the project's
# source list is globbed at configure time, so configuring against an empty
# generated/ produces a binary with no guest code in it and a link that fails on
# PPCImageConfig. The build's own codegen step then writes the files too late.
echo "==> generating guest code"
(cd "$PROJECT_ROOT" && "$REXGLUE" codegen | tail -2)

if [ "$PACK" = 1 ]; then
  echo "==> packing content into one zip"
  (cd "$PROJECT_ROOT" && tools/content_zip.sh pack)
fi

(cd "$PROJECT_ROOT" && git init -q && git add -A && \
  git -c user.name=CrownParkComputing -c user.email=jonathanmarkwhittingham@gmail.com \
    commit -q -m "Scaffold $NAME from the standard port template")

cat <<NEXT

$PROJECT_ROOT is ready. Next:

  cd $PROJECT_ROOT
  cmake -S . -B out/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \\
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++
  cmake --build out/build/linux -j\$(nproc)
  ./run.sh
NEXT
