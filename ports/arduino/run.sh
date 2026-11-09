#!/usr/bin/env bash
# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

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
  -DR2S_TESTING -DR2S_TEST_TX_CAP=16 \
  -I"$root/ports/arduino/tests/stubs" \
  -I"$root/ports/arduino" \
  -I"$root/core/include" \
  "$root/ports/arduino/r2s_arduino_port.cpp" \
  "$root/ports/arduino/tests/test_arduino_port.cpp" \
  "$root/ports/arduino/tests/stubs/Arduino.cpp" \
  -o "$build/test_arduino_port" || exit 1
[ "$build_only" = 1 ] && exit 0
"$build/test_arduino_port" || exit 1
