#!/usr/bin/env bash
# Rerun core unit tests by name: ctest-one.sh '<regex>'. Builds the gcc config
# only, then runs the matching tests there, so a bug fixer reruns just the
# failing test without the port legs the full CORE scope adds.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
[ $# -ge 1 ] || { echo "usage: ctest-one.sh '<regex>'"; exit 2; }
# The build directory mirrors scripts/core-test.sh; the pin in
# scripts/test_everyday.py keeps the two defaults identical.
build="${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/core"
bash "$root/scripts/core-test.sh" --build-only gcc || exit 1
ctest --test-dir "$build/gcc" --output-on-failure --schedule-random -R "$1"
