#!/usr/bin/env bash
# Build and test the C++ layer on the host in each configuration CI checks:
# gcc and clang, and clang with the sanitizers. Builds go under
# R2S_BUILD_DIR. Usage: cpp-test.sh [--build-only] [CONFIG...]
#   (default: every config)
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/cpp
all=(gcc clang sanitize)

# One lock for the whole cpp build directory, held for the entire run.
if [ -z "${R2S_CPP_LOCKED:-}" ]; then
  mkdir -p "$build"
  exec env R2S_CPP_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/scripts/cpp-test.sh" "$@"
fi

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
configs=("$@")
[ "${#configs[@]}" -gt 0 ] || configs=("${all[@]}")

flags() {
  case $1 in
    gcc) echo "-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=MinSizeRel" ;;
    clang) echo "-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release" ;;
    sanitize) echo "-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DR2S_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug" ;;
    *) return 1 ;;
  esac
}

run_config() {
  local name=$1 dir="$build/$1" args
  args=$(flags "$name") || { echo "unknown configuration $name; known: ${all[*]}"; return 2; }
  # shellcheck disable=SC2086 # args holds several -D options.
  cmake -S cpp -B "$dir" -G Ninja $args >"$dir.configure.log" 2>&1 \
    || { cat "$dir.configure.log"; return 1; }
  cmake --build "$dir" || return 1
  [ "$build_only" = 1 ] && return 0
  ctest --test-dir "$dir" --output-on-failure --schedule-random || return 1
}

failed=()
for c in "${configs[@]}"; do
  echo "-- cpp: $c"
  if run_config "$c"; then
    echo "-- cpp: $c ok"
  else
    echo "-- cpp: $c FAILED"
    failed+=("$c")
  fi
done
# The headers the board compiles are checked with the toolchain that
# builds them, so a change only the AVR compiler rejects fails here.
if [ "$build_only" = 0 ]; then
  echo "-- cpp: avr-syntax"
  if bash "$root/cpp/tests/avr-syntax.sh"; then
    echo "-- cpp: avr-syntax ok"
  else
    echo "-- cpp: avr-syntax FAILED"
    failed+=("avr-syntax")
  fi
fi
if [ "${#failed[@]}" -gt 0 ]; then
  echo "cpp configurations failed: ${failed[*]}"
  exit 1
fi
