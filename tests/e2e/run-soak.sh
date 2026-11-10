#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The nightly soak: clean-link traffic on posix and simavr, two hours per
# backend on the schedule. Builds the bridge workspace, both firmwares and
# the simavr harness under R2S_BUILD_DIR, then runs one soak leg per backend
# with a single RMW: one RMW keeps the nightly box at one window per backend,
# and the PR matrix already covers the RMW differences. R2S_SOAK_SECONDS sets
# the window per backend (7200 on the schedule, minutes for a local run) and
# R2S_SOAK_BACKENDS narrows the backends. The suite runs this for SOAK=1.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
# The timeout pin in scripts/test_job_timeouts.py reads this line, so the
# bound follows the backends without a second place to update.
backends=${R2S_SOAK_BACKENDS:-posix avr}
duration=${R2S_SOAK_SECONDS:-7200}
rmw=${R2S_RMWS:-rmw_fastrtps_cpp}
case "$rmw" in
  *" "*) echo "run-soak: one RMW at a time, got '$rmw'" >&2; exit 2 ;;
esac
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/e2e/soak
# One isolation slot per run: a leased DDS domain, so concurrent suites never
# share a domain. Held for the whole run, exactly like the build lock below.
if [ -z "${R2S_E2E_LEASED:-}" ]; then
  exec env R2S_E2E_LEASED=1 bash "$root/scripts/e2e-lease.sh" bash "$root/tests/e2e/run-soak.sh" "$@"
fi
# One lock for the soak build directory, held for the entire run: a direct
# run and a suite run of one tree must not write the same binaries.
if [ -z "${R2S_SOAK_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_SOAK_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/tests/e2e/run-soak.sh" "$@"
fi
mkdir -p "$build" || exit 1

command -v colcon >/dev/null 2>&1 || { echo "colcon not found: run it in a ROS container"; exit 1; }
: "${ROS_DISTRO:?ROS_DISTRO is not set: run it in a ROS container}"
if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/$ROS_DISTRO/setup.bash" ]; then
  # shellcheck source=/dev/null
  set +u
  source "/opt/ros/$ROS_DISTRO/setup.bash"
  set -u
fi
export R2S_BUILD_DIR=$build

bash scripts/bridge-test.sh --build-only || exit 1
# shellcheck source=/dev/null
set +u
source "$build/bridge/install/setup.bash"
set -u

bash ports/posix/run.sh --build-only || exit 1
bash tests/e2e/run-avr.sh --build-only || exit 1
export R2S_E2E_FW=$build/ports/posix/r2s_posix_firmware
export R2S_SIMAVR_BIN=$build/harness/simavr/r2s_simavr
export R2S_E2E_AVR_FW=$build/e2e/avr/fw

# The scenario files import the framework as a package; direct launch_test
# runs need the same directory on the import path.
export PYTHONPATH=$root/tests/e2e${PYTHONPATH:+:$PYTHONPATH}
# The soak gate runs before any backend, so a broken checker fails fast
# instead of after hours; the scenarios re-check the same gate on live data.
python3 tests/e2e/soak_check_test.py || exit 1

# One DDS domain per suite run, so parallel runs on the shared host never see
# each other's topics: every process below inherits the same domain.
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
echo "== soak isolation: ROS_DOMAIN_ID=$ROS_DOMAIN_ID"

status=0
for backend in $backends; do
  case $backend in
    posix) test_file=test_soak_posix ;;
    avr)
      test_file=test_soak_avr
      # The harness stops the firmware after its simulated budget, so the
      # budget covers the wall window plus headroom for sim time running hot.
      export R2S_SIMAVR_RUN_MS=$((duration * 1000 + 3600000))
      ;;
    *) echo "run-soak: unknown backend '$backend', want posix or avr" >&2; exit 2 ;;
  esac
  echo "== soak ($backend): $rmw for ${duration}s"
  export RMW_IMPLEMENTATION=$rmw
  # The margin covers the handshake and the drain past the window.
  timeout $((duration + 900)) launch_test "tests/e2e/${test_file}.py" || status=1
done
exit $status
