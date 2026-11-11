#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Build and run this directory's host test: the port contract against
# fake registers, compiled with the host C compiler. Every path derives
# from this script's location, so a renamed copy tests itself under its
# own name. Builds go under R2S_BUILD_DIR, so the tree stays clean.
# Usage: run.sh [--build-only]
set -uo pipefail
port_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
port=$(basename "$port_dir")
port_upper=$(echo "$port" | tr '[:lower:]' '[:upper:]')
root=$(cd "$port_dir/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/ports/$port
self="ports/$port/run.sh"
# One lock for the port build directory, held for the entire run: a direct
# run and a suite run of one tree must not write the same binary at once.
lock_var="R2S_${port_upper}_LOCKED"
if [ -z "${!lock_var:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env "$lock_var=1" bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$port_dir/run.sh" "$@"
fi
mkdir -p "$build" || exit 1

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: $self [--build-only]" >&2; exit 2; }

cc=${CC:-gcc}
"$cc" -std=c99 -Werror -Wall -Wextra -Wconversion -Wshadow \
  -Wstrict-prototypes -Wmissing-prototypes -Wundef \
  "-DR2S_${port_upper}_HOST_TEST" \
  -I"$port_dir" \
  -I"$root/core/include" \
  "$port_dir/r2s_${port}_port.c" \
  "$port_dir/tests/test_${port}_port.c" \
  -o "$build/test_${port}_port" || exit 1
[ "$build_only" = 1 ] && exit 0
"$build/test_${port}_port" || exit 1
