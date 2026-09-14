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
cp "$SDK/tools/content_zip.sh" "$SDK/tools/port_check.py" "$SDK/tools/port_doctor.sh" "$PROJECT_ROOT/tools/"
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

# The disc content is not in the repository, and importing it is not part of
# the build: a rebuild should not depend on having the game to hand. If assets/
# is missing or does not match content/content.sha256, ask for the archive -
# once - and import it.
if ! "\$ROOT/tools/content_zip.sh" verify >/dev/null 2>&1; then
  ZIP="\${${NAME^^}_CONTENT_ZIP:-}"
  DEFAULT="\$ROOT/content/$NAME-content.zip"
  if [ -z "\$ZIP" ] && [ -f "\$DEFAULT" ]; then
    ZIP="\$DEFAULT"
  fi
  if [ -z "\$ZIP" ]; then
    if [ -t 0 ]; then
      echo "Game content for $NAME is not installed."
      read -r -p "Path to the content zip: " ZIP
    else
      echo "game content missing; set ${NAME^^}_CONTENT_ZIP to the archive" >&2
      exit 1
    fi
  fi
  ZIP="\${ZIP/#\\~/\$HOME}"
  [ -f "\$ZIP" ] || { echo "no archive at \$ZIP" >&2; exit 1; }
  "\$ROOT/tools/content_zip.sh" restore "\$ZIP"
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

\`assets/\` holds the disc files and is never in Git. \`tools/content_zip.sh
pack\` packs it into one stored zip under \`content/\` - also never committed -
with \`content/content.sha256\` (which IS committed) recording the archive
checksum and every file's.

Importing the content is not part of the build: a rebuild must not depend on
having the game to hand. \`./run.sh\` checks \`assets/\` against those checksums
and, only when they do not match, asks where the archive is - or takes
\`${NAME^^}_CONTENT_ZIP=/path/to/$NAME-content.zip\`.

## First run: let the doctor pick the settings

\`\`\`sh
tools/port_doctor.sh .        # ~6 minutes, no display, nobody at the keyboard
\`\`\`

Checks the generated code, then runs the title on each render path and each
page-coherency setting and reports what each one costs. Every check exists
because a title failed it and the cause took hours to find by hand: Shift 2
loses its text on the \`host\` path, Hydro Thunder renders blocky tile-pattern
garbage with \`clear_memory_page_state=false\`, MCLA is 25% faster on \`host\`.
Nothing about the symptom says which - compare the frames it captures.

## Testing without a display

\`\`\`sh
tools/headless_play.sh 150 "25:Return 100:e:35"   # capture frames, drive menus
tools/measure.sh 150 "25:Return"                  # per-frame cost breakdown
tools/measure.sh 150 "25:Return" --gpu_hot_page_frames=0   # A/B a setting
\`\`\`

## Replacing guest code with native code

Any recompiled function can be replaced with a native C++ one. \`DEFINE_REX_FUNC\`
emits the body as \`__imp__<name>\` and \`<name>\` as a weak alias, so a strong
definition in \`src/\` wins at link time and the original stays callable:

\`\`\`cpp
#include <rex/hook.h>
extern "C" void __imp__sub_82345678(PPCContext& ctx, uint8_t* base);

REX_HOOK_RAW(sub_82345678) {
  // measure, replace, or skip - then optionally run the original
  __imp__sub_82345678(ctx, base);
}
\`\`\`

Add the file with \`target_sources(<title>_recomp PRIVATE ...)\` AFTER
\`rexglue_setup_target\`, which is what creates that target. This survives a
re-codegen, which a hand edit to \`generated/\` does not - that is the whole
reason the mechanism exists. \`REX_HOOK(name, fn)\` marshals PPC registers into
plain C++ arguments; \`[[midasm_hook]]\` in the manifest injects a native call at
one instruction inside a function.

Worth knowing what this is NOT for: replacing a title's audio or physics
middleware wholesale has been tried and does not work, because the state that
matters (voice position, gain, pan) lives inside the guest and is not visible
from outside it. Use it to measure, to skip a path that cannot work, or to fix
one function - not to reimplement a subsystem.

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

# Check the output before anyone spends minutes compiling it or hours playing
# it. Both of the expensive failures on previous conversions are visible here:
# a function the scanner split so the halves branch into each other (does not
# compile, found after a four minute build) and a call to an address codegen
# never emitted (compiles fine, then kills the title the first time that path
# runs). A non-zero exit prints the manifest lines to paste.
echo "==> checking generated code"
if ! "$PROJECT_ROOT/tools/port_check.py" "$PROJECT_ROOT"; then
  echo
  echo "Add those lines to ${NAME}_manifest.toml, re-run:"
  echo "  $REXGLUE codegen ${NAME}_manifest.toml"
  echo "and check again before building."
fi

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
