#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Run one pytest selection in the image: pytest-one.sh '<path>[::name] [-k expr]'.
# The host library builds first, as the GEN scope does, so the model tests
# run against the current core.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
[ $# -ge 1 ] || { echo "usage: pytest-one.sh '<path>[::name] [-k expr]'"; exit 2; }
host_dir="${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/host"
bash "$root/scripts/build-host-lib.sh" "$host_dir" || exit 1
# shellcheck disable=SC2086 # the selection is a path plus pytest flags, one word each.
R2S_HOST_LIB_DIR="$host_dir" python3 -m pytest $1
