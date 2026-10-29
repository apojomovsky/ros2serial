#!/usr/bin/env bash
# Build and test every bridge package in the ROS environment the caller runs
# in: the dev image locally, the pinned distro container in CI. The
# workspace lives under the shared build directory, outside the checkout.
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
build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: scripts/bridge-test.sh [--build-only]"; exit 2; }
workspace=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/bridge
# One isolation slot per run: a leased DDS domain, so concurrent suites never
# see each other's nodes. Held for the whole run, exactly like the build
# lock below.
if [ -z "${R2S_E2E_LEASED:-}" ]; then
  exec env R2S_E2E_LEASED=1 bash "$root/scripts/e2e-lease.sh" bash "$root/scripts/bridge-test.sh" "$@"
fi
# One lock for the bridge workspace, held for the entire run: a direct run
# and a suite run of one tree must not write the same binaries.
if [ -z "${R2S_BRIDGE_LOCKED:-}" ]; then
  mkdir -p "$workspace" || exit 1
  exec env R2S_BRIDGE_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$workspace/.lock" \
    bash "$root/scripts/bridge-test.sh" "$@"
fi
mkdir -p "$workspace" || exit 1
# One DDS domain per suite run, so parallel runs on the shared host never see
# each other's nodes: every process below inherits the same domain.
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
echo "== bridge isolation: ROS_DOMAIN_ID=$ROS_DOMAIN_ID"
echo "== bridge: $ROS_DISTRO"

colcon --log-base "$workspace/log" build \
  --base-paths bridge \
  --build-base "$workspace/build" \
  --install-base "$workspace/install" \
  --event-handlers console_direct+ || exit 1
[ "$build_only" = 1 ] && exit 0

# shellcheck source=/dev/null
set +u
source "$workspace/install/setup.bash"
set -u
status=0
colcon --log-base "$workspace/log" test \
  --base-paths bridge \
  --build-base "$workspace/build" \
  --install-base "$workspace/install" \
  --event-handlers console_direct+ \
  --return-code-on-test-failure || status=1
colcon --log-base "$workspace/log" test-result --test-result-base "$workspace/build" --all --verbose || status=1
# The generator's hashes equal the installed type support's for every bundled
# type on this distro (docs/01-testing.md, section 9): pytest in this container.
python3 -m pytest gen/tests/test_installed_hashes.py || status=1
exit $status
