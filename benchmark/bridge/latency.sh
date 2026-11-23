#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Measure the bridge's host-to-MCU latency over a pty pair, in the ROS
# environment the caller runs in: the dev image locally, CI's distro
# container otherwise. Builds stay under the shared build directory, so the
# tree stays clean. Usage: benchmark/bridge/latency.sh [--samples N]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
command -v colcon >/dev/null 2>&1 || { echo "colcon not found: run it in a ROS container"; exit 1; }
: "${ROS_DISTRO:?ROS_DISTRO is not set: run it in a ROS container}"
base=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}
build=$base/benchmark/bridge
# One lock for the benchmark's workspace, held for the whole run: a direct run
# and a suite run of one tree must not write the same binaries.
if [ -z "${R2S_BENCH_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_BENCH_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/benchmark/bridge/latency.sh" "$@"
fi
mkdir -p "$build" || exit 1
# One isolation slot per run: a leased DDS domain, so the probe sees this
# bridge and no other suite's nodes on a shared host.
if [ -z "${R2S_E2E_LEASED:-}" ]; then
  exec env R2S_E2E_LEASED=1 R2S_E2E_DOMAIN_BASE=${R2S_E2E_DOMAIN_BASE:-0} \
    bash "$root/scripts/e2e-lease.sh" bash "$root/benchmark/bridge/latency.sh" "$@"
fi
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}

set +u
source "/opt/ros/$ROS_DISTRO/setup.bash"
set -u

# The bridge and the firmware both come from this tree, and the benchmark
# measures the node process alone: a stale install would measure other code.
bash "$root/scripts/bridge-test.sh" --build-only || exit 1
bash "$root/ports/posix/run.sh" --build-only || exit 1
set +u
source "$base/bridge/install/setup.bash"
set -u

exec python3 "$root/benchmark/bridge/outbound_latency.py" \
  --bridge "$base/bridge/install/r2s_bridge/lib/r2s_bridge/bridge" \
  --firmware "$base/ports/posix/r2s_posix_firmware" "$@"
