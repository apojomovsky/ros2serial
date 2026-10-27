#!/usr/bin/env bash
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

cmake -S ports/posix -B "$build" -G Ninja >"$build.configure.log" 2>&1 \
  || { cat "$build.configure.log"; exit 1; }
cmake --build "$build" || exit 1
[ "$build_only" = 1 ] && exit 0
ctest --test-dir "$build" --output-on-failure || exit 1
R2S_POSIX_FW="$build/r2s_posix_firmware" python3 -m pytest ports/posix/tests -q
