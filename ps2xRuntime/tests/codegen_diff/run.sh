#!/usr/bin/env bash
# Differential test of the legacy vs. new (scalar-GPR + fast-RAM) generated-code macros.
#   run.sh BUILD_DIR [MAX_FUNCS=450] [SEEDS=16] [extra select_leaves.py args, e.g. --strict]
# Builds one test source twice (-DPS2X_LEGACY_ACCESS and default) into BUILD_DIR/{legacy,new}/difftest,
# runs both, diffs the per-(function,seed) hash lines, prints PASS/FAIL. Reads, never writes, the repo.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${1:?usage: run.sh BUILD_DIR [MAX_FUNCS] [SEEDS]}"
MAXF="${2:-450}"
SEEDS="${3:-16}"
shift $(( $# > 3 ? 3 : $# )) || true
CC_DB="${CC_DB:-$ROOT/build-mac-runtime/compile_commands.json}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
mkdir -p "$BUILD/gen" "$BUILD/legacy" "$BUILD/new"

FLAGS="$(python3 "$HERE/flags.py" "$CC_DB")"
python3 "$HERE/select_leaves.py" --runner "$ROOT/ps2xRuntime/src/runner" --out "$BUILD/gen" --max "$MAXF" "$@"

build_variant() { # name extra-flags
  local v="$1" extra="$2" objs=()
  local jobs_file="$BUILD/$v/jobs.txt"; : > "$jobs_file"
  for src in "$BUILD"/gen/chunk_*.cpp "$HERE/test.cpp"; do
    local o="$BUILD/$v/$(basename "$src" .cpp).o"; objs+=("$o")
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ "$HERE/harness.h" -nt "$o" ] || [ "$ROOT/ps2xRuntime/include/ps2_runtime_macros.h" -nt "$o" ] || [ "$BUILD/gen/chunk_table.inc" -nt "$o" ]; then
      echo "c++ $FLAGS $extra -O2 -w -I$HERE -I$BUILD/gen -c $src -o $o" >> "$jobs_file"
    fi
  done
  echo "[$v] compiling $(wc -l < "$jobs_file") TUs with -P$JOBS"
  tr '\n' '\0' < "$jobs_file" | xargs -0 -n1 -P "$JOBS" sh -c 'eval "$0"'
  c++ -arch arm64 "${objs[@]}" -o "$BUILD/$v/difftest"
}
build_variant legacy "-DPS2X_LEGACY_ACCESS"
build_variant new ""

for v in legacy new; do
  echo "[$v] running"
  ( time "$BUILD/$v/difftest" --seeds "$SEEDS" ${DIFFTEST_ARGS:-} > "$BUILD/$v/out.txt" ) 2>&1 | grep -E 'variant|real' || true
  sort -o "$BUILD/$v/out.txt" "$BUILD/$v/out.txt"
done
set +e
python3 "$HERE/compare.py" "$BUILD/legacy/out.txt" "$BUILD/new/out.txt" | tee "$BUILD/report.txt"
rc=${PIPESTATUS[0]}
if [ "$rc" != 0 ]; then
  # Informational: model the 16-byte RAM padding as the wrapped start of RAM (legacy wraps accesses that
  # straddle the end of RAM, the fast path reads/writes the padding). Shows what is left over.
  for v in legacy new; do
    "$BUILD/$v/difftest" --seeds "$SEEDS" --emulate-wrap 2>/dev/null | sort > "$BUILD/$v/out_wrap.txt"
  done
  echo "---- same comparison with --emulate-wrap (end-of-RAM wrap modelled; informational) ----"
  python3 "$HERE/compare.py" "$BUILD/legacy/out_wrap.txt" "$BUILD/new/out_wrap.txt" | grep -v '^drop reasons' | tee -a "$BUILD/report.txt"
fi
exit "$rc"
