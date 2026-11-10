#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Build the minimal CH32V003 firmware through ports/ch32v/build.sh. The
# RISC-V toolchain lives in the dev image at the version pinned in
# ci/versions.env, never in a cache directory. Builds go under
# R2S_BUILD_DIR, so the tree stays clean.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/examples
# One lock for the examples build directory, held for the whole run: a direct
# run and a suite run of one tree must not write the same ELF at once.
if [ -z "${R2S_EXAMPLES_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_EXAMPLES_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/scripts/build-examples-ch32v.sh" "$@"
fi
mkdir -p "$build" || exit 1

bash "$root/ports/ch32v/build.sh" --out "$build/ch32v_minimal/ch32v" || exit 1
