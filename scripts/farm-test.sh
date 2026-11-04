#!/usr/bin/env bash
# Farm-shaped check: build and test the released packages outside the dev image.
#
# The ROS build farm builds from a clean chroot with no Docker and no network
# at build time, so this script runs no Docker, no fetch and no host-only
# tool: only colcon, the ROS underlay and the checkout. CI runs it in a
# clean ROS base image (farm.yml); locally the dev image also serves, since
# it is a superset. The dev-image suite stays; this stands alongside it.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
command -v colcon >/dev/null 2>&1 || { echo "colcon not found: run it in a ROS container"; exit 1; }
: "${ROS_DISTRO:?ROS_DISTRO is not set: run it in a ROS container}"
if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/$ROS_DISTRO/setup.bash" ]; then
  # shellcheck source=/dev/null
  set +u
  source "/opt/ros/$ROS_DISTRO/setup.bash"
  set -u
fi
[ "$#" -eq 0 ] || { echo "usage: scripts/farm-test.sh"; exit 2; }
workspace=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/farm
# One isolation slot per run: a leased DDS domain, so concurrent suites never
# see each other's nodes. Held for the whole run, exactly like the bridge
# job, whose launch test joins real nodes.
if [ -z "${R2S_E2E_LEASED:-}" ]; then
  exec env R2S_E2E_LEASED=1 R2S_E2E_DOMAIN_BASE=${R2S_E2E_DOMAIN_BASE:-0} \
    bash "$root/scripts/e2e-lease.sh" bash "$root/scripts/farm-test.sh"
fi
# One lock for the farm workspace, held for the entire run: a direct run and
# a suite run of one tree must not write the same binaries.
if [ -z "${R2S_FARM_LOCKED:-}" ]; then
  mkdir -p "$workspace" || exit 1
  exec env R2S_FARM_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$workspace/.lock" \
    bash "$root/scripts/farm-test.sh"
fi
mkdir -p "$workspace" || exit 1
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
echo "== farm isolation: ROS_DOMAIN_ID=$ROS_DOMAIN_ID"
echo "== farm: $ROS_DISTRO"

# The generator's model tests drive the real core through this library, the
# same fixture the GEN suite section builds before pytest runs. Built here so
# the colcon test step finds it through the environment below.
bash "$root/scripts/build-host-lib.sh" "$workspace/host" || exit 1
export R2S_HOST_LIB_DIR="$workspace/host"

# gen/ itself is the r2s_gen package, so a plain --base-paths gen would hide
# the nested gen/r2s_rosidl package: every released package is named.
colcon --log-base "$workspace/log" build \
  --base-paths gen gen/r2s_rosidl bridge/r2s_bridge bridge/r2s_bridge_tests \
  --build-base "$workspace/build" \
  --install-base "$workspace/install" \
  --event-handlers console_direct+ || exit 1

# shellcheck source=/dev/null
set +u
source "$workspace/install/setup.bash"
set -u
status=0
colcon --log-base "$workspace/log" test \
  --base-paths gen gen/r2s_rosidl bridge/r2s_bridge bridge/r2s_bridge_tests \
  --build-base "$workspace/build" \
  --install-base "$workspace/install" \
  --event-handlers console_direct+ \
  --return-code-on-test-failure || status=1
colcon --log-base "$workspace/log" test-result --test-result-base "$workspace/build" --all --verbose || status=1
[ "$status" -eq 0 ] || exit "$status"

# Smoke the installed entry points: the generator reports the repository
# version, and the bridge package installed its share and component.
version=$(tr -d '[:space:]' <"$root/VERSION")
generator="$workspace/install/r2s_gen/lib/r2s_gen/r2s-gen"
[ -x "$generator" ] || { echo "farm: no installed r2s-gen at $generator"; exit 1; }
printed=$("$generator" version) || exit 1
echo "$printed"
echo "$printed" | grep -q "r2s-gen $version" || { echo "farm: installed r2s-gen is not $version"; exit 1; }
[ -d "$workspace/install/r2s_bridge/share/r2s_bridge" ] || { echo "farm: no installed r2s_bridge share"; exit 1; }
[ -f "$workspace/install/r2s_rosidl/share/r2s_rosidl/cmake/r2s_generate_firmware.cmake" ] \
  || { echo "farm: no installed r2s_rosidl helper"; exit 1; }
echo "== farm: ok"
