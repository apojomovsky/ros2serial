#!/usr/bin/env bash
# Build and test the core on the host in each configuration CI checks: gcc,
# clang, gcc -m32, clang with ASan and UBSan, and gcc with coverage held to the
# floors in docs/01-testing.md, section 6. Builds go under R2S_BUILD_DIR.
# Usage: core-test.sh [--build-only] [CONFIG...]   (default: every config)
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/core
all=(gcc clang gcc-m32 sanitize coverage)
LINE_FLOOR=95
BRANCH_FLOOR=90

# One lock for the whole core build directory, held for the entire run.
if [ -z "${R2S_CORE_LOCKED:-}" ]; then
  mkdir -p "$build"
  exec env R2S_CORE_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/scripts/core-test.sh" "$@"
fi

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
configs=("$@")
[ "${#configs[@]}" -gt 0 ] || configs=("${all[@]}")

flags() {
  case $1 in
    # Optimized builds, as on the targets, so the warnings that need the
    # optimizer's analysis fire too; the instrumented builds stay at -O0.
    gcc) echo "-DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=MinSizeRel" ;;
    clang) echo "-DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release" ;;
    gcc-m32) echo "-DCMAKE_C_COMPILER=gcc -DCMAKE_C_FLAGS=-m32 -DCMAKE_BUILD_TYPE=Debug" ;;
    sanitize) echo "-DCMAKE_C_COMPILER=clang -DR2S_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug" ;;
    coverage) echo "-DCMAKE_C_COMPILER=gcc -DR2S_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug" ;;
    *) return 1 ;;
  esac
}

coverage_report() {
  local dir=$1
  if [ -z "$(find core/src -name '*.c' -print -quit)" ]; then
    echo "coverage: no sources in core/src yet, nothing to measure"
    return 0
  fi
  gcovr --root "$root" --filter 'core/src/' --object-directory "$dir" \
    --txt-metric branch --print-summary --exclude-unreachable-branches \
    --fail-under-line "$LINE_FLOOR" --fail-under-branch "$BRANCH_FLOOR" "$dir"
}

run_config() {
  local name=$1 dir="$build/$1" args
  args=$(flags "$name") || { echo "unknown configuration $name; known: ${all[*]}"; return 2; }
  if [ "$name" = gcc-m32 ] && [ "$(uname -m)" != x86_64 ]; then
    echo "SKIPPED: -m32 needs an x86_64 host (this is $(uname -m)); CI runs it"
    return 0
  fi
  # shellcheck disable=SC2086 # args holds several -D options.
  # A coverage build starts empty: notes left by a deleted source make gcovr abort.
  if [ "$name" = coverage ]; then rm -rf "$dir"; fi
  cmake -S core -B "$dir" -G Ninja $args >"$dir.configure.log" 2>&1 \
    || { cat "$dir.configure.log"; return 1; }
  cmake --build "$dir" || return 1
  [ "$build_only" = 1 ] && return 0
  ctest --test-dir "$dir" --output-on-failure || return 1
  if [ "$name" = coverage ]; then coverage_report "$dir" || return 1; fi
}

failed=()
for c in "${configs[@]}"; do
  echo "-- core: $c"
  if run_config "$c"; then
    echo "-- core: $c ok"
  else
    echo "-- core: $c FAILED"
    failed+=("$c")
  fi
done
if [ "${#failed[@]}" -gt 0 ]; then
  echo "core configurations failed: ${failed[*]}"
  exit 1
fi
