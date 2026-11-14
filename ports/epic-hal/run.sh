#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Build and run the PIC18F4550 glue's host test: the port contract against
# fake SFRs, compiled with the host C compiler. Builds go under
# R2S_BUILD_DIR, so the tree stays clean. Usage: run.sh [--build-only]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/ports/epic-hal
# One lock for the epic-hal build directory, held for the entire run: a
# direct run and a suite run of one tree must not write the same binary.
if [ -z "${R2S_EPIC_HAL_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_EPIC_HAL_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/ports/epic-hal/run.sh" "$@"
fi
mkdir -p "$build" || exit 1

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: ports/epic-hal/run.sh [--build-only]" >&2; exit 2; }

cc=${CC:-gcc}
# The suite builds the glue at the default tick rate, and the simulator
# compiles it at 1 MHz to match its vector, so both are built and run here:
# a rate the tests never compile is a rate nothing checks.
for rate in 1000 1000000; do
  "$cc" -std=c99 -Werror -Wall -Wextra -Wconversion -Wshadow \
    -Wstrict-prototypes -Wmissing-prototypes -Wundef \
    -DR2S_PIC18F4550_HOST_TEST -DR2S_TICK_HZ="$rate" \
    -I"$root/ports/epic-hal" \
    -I"$root/core/include" \
    "$root/ports/epic-hal/r2s_pic18f4550_port.c" \
    "$root/ports/epic-hal/tests/test_pic18f4550_port.c" \
    -o "$build/test_pic18f4550_port_$rate" || exit 1
done
[ "$build_only" = 1 ] && exit 0
for rate in 1000 1000000; do
  "$build/test_pic18f4550_port_$rate" || exit 1
done
