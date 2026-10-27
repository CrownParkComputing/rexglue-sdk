#!/usr/bin/env bash
# A/B the codegen flags against the qemu-derived PPC instruction expectations.
#
# Builds one ppc_tests executable per flag set (see REXGLUE_PPC_TEST_VARIANTS in
# tests/ppc/CMakeLists.txt) from the same tests/ppc/asm set, runs each, and
# prints a per-variant pass/fail table plus the tests whose verdict differs
# from the default codegen. A flag is "instruction-neutral" when its column
# equals the default column; every differing test is listed so it can be
# classified (expected: a test observing r14-r31/f14-f31/v14-v31,v64-v127 under
# non_volatile_as_local, which the harness cannot see once localized).
#
# Usage:
#   tools/ppc_oracle/ab_flags.sh [-B build_dir] [-c Release] [variant ...]
#   variants default to: default non_volatile_as_local nonvolatile_memory
#                        non_volatile_as_local+nonvolatile_memory
# Environment:
#   CMAKE_ARGS   extra configure arguments (e.g. -G Ninja -DCMAKE_BUILD_TYPE=Release)
#   JOBS         parallel build jobs (default: nproc)
#
# Results land in <build_dir>/ppc_ab/<variant>.txt (one "PASS|FAIL <test>" line
# per test, sorted) and <build_dir>/ppc_ab/summary.md.
set -euo pipefail

SDK="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$SDK/out/build/ppc-ab"
CONFIG="Release"
while getopts "B:c:h" opt; do
  case "$opt" in
    B) BUILD="$OPTARG" ;;
    c) CONFIG="$OPTARG" ;;
    h) sed -n '2,24p' "$0"; exit 0 ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
VARIANTS=("$@")
if [ ${#VARIANTS[@]} -eq 0 ]; then
  VARIANTS=(default non_volatile_as_local nonvolatile_memory non_volatile_as_local+nonvolatile_memory)
fi
JOBS="${JOBS:-$(nproc)}"

variant_list="$(IFS=';'; echo "${VARIANTS[*]}")"
echo "== configure $BUILD ($CONFIG) variants: $variant_list"
# shellcheck disable=SC2086
cmake -S "$SDK" -B "$BUILD" ${CMAKE_ARGS:-} \
  -DREXGLUE_BUILD_TESTS=ON \
  -DREXGLUE_PPC_TEST_VARIANTS="$variant_list" >/dev/null

targets=()
for v in "${VARIANTS[@]}"; do
  if [ "$v" = default ]; then targets+=(ppc_tests); else targets+=("ppc_tests_${v//+/_}"); fi
done
echo "== build ${targets[*]}"
cmake --build "$BUILD" --config "$CONFIG" -j"$JOBS" --target "${targets[@]}" >/dev/null

OUT="$BUILD/ppc_ab"; mkdir -p "$OUT"
run_variant() { # $1 = variant, $2 = target
  local exe
  exe="$(find "$BUILD" "$SDK/out" -type f -name "$2" -perm -u+x 2>/dev/null | grep -E "/$CONFIG/|/bin/|/$2\$" | head -1)"
  [ -n "$exe" ] || { echo "cannot find $2 under $BUILD" >&2; return 1; }
  # One process per test case so a crash in one instruction cannot hide the
  # verdicts of the others (Catch2 stops reporting after an abort).
  "$exe" --list-tests --verbosity quiet 2>/dev/null | while IFS= read -r name; do
    if "$exe" "$name" >/dev/null 2>&1; then echo "PASS $name"; else echo "FAIL $name"; fi
  done | sort > "$OUT/$1.txt"
}

i=0
for v in "${VARIANTS[@]}"; do
  echo "== run ${targets[$i]} -> $OUT/$v.txt"
  run_variant "$v" "${targets[$i]}"
  i=$((i + 1))
done

{
  echo "# ppc instruction tests by codegen flag set"
  echo
  echo "| variant | pass | fail | differs from default |"
  echo "|---|---|---|---|"
  for v in "${VARIANTS[@]}"; do
    pass=$(grep -c '^PASS' "$OUT/$v.txt" || true)
    fail=$(grep -c '^FAIL' "$OUT/$v.txt" || true)
    if [ -f "$OUT/default.txt" ]; then
      diffs=$(comm -3 "$OUT/default.txt" "$OUT/$v.txt" | wc -l)
    else
      diffs="n/a"
    fi
    echo "| $v | $pass | $fail | $diffs |"
  done
  echo
  for v in "${VARIANTS[@]}"; do
    [ "$v" = default ] && continue
    [ -f "$OUT/default.txt" ] || continue
    if ! cmp -s "$OUT/default.txt" "$OUT/$v.txt"; then
      echo "## $v vs default"
      echo '```'
      diff "$OUT/default.txt" "$OUT/$v.txt" | grep '^[<>]' || true
      echo '```'
      echo
    fi
  done
} > "$OUT/summary.md"
cat "$OUT/summary.md"
