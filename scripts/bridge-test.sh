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
workspace=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/bridge
mkdir -p "$workspace" || exit 1
echo "== bridge: $ROS_DISTRO"

colcon --log-base "$workspace/log" build \
  --base-paths bridge \
  --build-base "$workspace/build" \
  --install-base "$workspace/install" \
  --event-handlers console_direct+ || exit 1

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
exit $status
