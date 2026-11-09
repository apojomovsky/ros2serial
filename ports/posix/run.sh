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

# Build the posix test firmware and run its contract tests. Builds go under
# R2S_BUILD_DIR, so the tree stays clean; the core suite runs this script.
# Usage: run.sh [--build-only]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/ports/posix
# One lock for the posix build directory, held for the entire run: a direct
# run and a suite run of one tree must not write the same binary.
if [ -z "${R2S_POSIX_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_POSIX_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/ports/posix/run.sh" "$@"
fi
mkdir -p "$build" || exit 1

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: ports/posix/run.sh [--build-only]" >&2; exit 2; }

generator_args=()
# The dev image carries ninja; images without it (lyrical CI) fall back to
# the default generator, so the firmware builds in both.
command -v ninja >/dev/null 2>&1 && generator_args=(-G Ninja)
cmake -S ports/posix -B "$build" "${generator_args[@]}" >"$build.configure.log" 2>&1 \
  || { cat "$build.configure.log"; exit 1; }
cmake --build "$build" || exit 1
[ "$build_only" = 1 ] && exit 0
ctest --test-dir "$build" --output-on-failure || exit 1
R2S_POSIX_FW="$build/r2s_posix_firmware" python3 -m pytest ports/posix/tests -q
