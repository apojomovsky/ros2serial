#!/usr/bin/env bash
# Build and run the Arduino glue's host test: the port contract against stub
# streams, compiled with the host C++ compiler. Builds go under R2S_BUILD_DIR,
# so the tree stays clean. Usage: run.sh [--build-only]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/ports/arduino
# One lock for the arduino build directory, held for the entire run: a direct
# run and a suite run of one tree must not write the same binary at once.
if [ -z "${R2S_ARDUINO_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_ARDUINO_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/ports/arduino/run.sh" "$@"
fi
mkdir -p "$build" || exit 1

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: ports/arduino/run.sh [--build-only]" >&2; exit 2; }

cxx=${CXX:-g++}
"$cxx" -std=c++17 -Werror -Wall -Wextra -Wconversion -Wshadow \
  -DR2S_TESTING \
  -I"$root/ports/arduino/tests/stubs" \
  -I"$root/ports/arduino" \
  -I"$root/core/include" \
  "$root/ports/arduino/r2s_arduino_port.cpp" \
  "$root/ports/arduino/tests/test_arduino_port.cpp" \
  "$root/ports/arduino/tests/stubs/Arduino.cpp" \
  -o "$build/test_arduino_port" || exit 1
[ "$build_only" = 1 ] && exit 0
"$build/test_arduino_port" || exit 1
