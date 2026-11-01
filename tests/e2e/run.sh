#!/usr/bin/env bash
# The posix end-to-end suite: scenarios 1 to 4 against the posix virtual MCU.
# Builds the bridge workspace and the firmware, then runs every scenario under
# each RMW the design names, starting the Zenoh router first for its leg.
# Builds go under R2S_BUILD_DIR, so the tree stays clean; the suite runs this
# script for E2E=posix. R2S_RMWS overrides the RMW list for a quicker loop.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/e2e/posix
# One isolation slot per run: a leased DDS domain and Zenoh router port, so
# concurrent suites never share a domain. Held for the whole run, exactly
# like the build lock below.
if [ -z "${R2S_E2E_LEASED:-}" ]; then
  exec env R2S_E2E_LEASED=1 bash "$root/scripts/e2e-lease.sh" bash "$root/tests/e2e/run.sh" "$@"
fi
# One lock for the e2e build directory, held for the entire run: a direct run
# and a suite run of one tree must not write the same binaries.
if [ -z "${R2S_E2E_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_E2E_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/tests/e2e/run.sh" "$@"
fi
mkdir -p "$build" || exit 1

rmws=${R2S_RMWS:-rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_zenoh_cpp}
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

# Both registration paths run the same scenarios: the generated C firmware
# and the runtime C++ firmware announce the same topics, so one scenario
# file covers both legs through R2S_E2E_FW.
bash ports/posix/run.sh --build-only || exit 1

# The scenario files import the framework as a package; direct launch_test
# runs need the same directory on the import path.
export PYTHONPATH=$root/tests/e2e${PYTHONPATH:+:$PYTHONPATH}
# The helper assertions run before any scenario, so a broken helper fails
# fast instead of mid-handshake; the script directory is already on the
# import path when Python runs the file directly.
python3 tests/e2e/checks_test.py || exit 1


# One DDS domain per suite run, so parallel runs on the shared host never see
# each other's topics: every process below inherits the same domain.
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
echo "== e2e isolation: ROS_DOMAIN_ID=$ROS_DOMAIN_ID ZENOH_ROUTER_PORT=$R2S_ZENOH_ROUTER_PORT"

status=0
for leg in static runtime; do
  case $leg in
    static) export R2S_E2E_FW=$build/ports/posix/r2s_posix_firmware ;;
    runtime) export R2S_E2E_FW=$build/ports/posix/r2s_posix_runtime_firmware ;;
  esac
  for rmw in $rmws; do
    echo "== e2e-posix ($leg): $rmw"
    export RMW_IMPLEMENTATION=$rmw
    router=""
    if [ "$rmw" = rmw_zenoh_cpp ]; then
      router=$(find "/opt/ros/$ROS_DISTRO" -name rmw_zenohd -type f 2>/dev/null | head -n 1)
      [ -n "$router" ] || { echo "rmw_zenohd not found for $ROS_DISTRO"; status=1; break; }
      # A router per run on the leased port, and every node pointed at it, so
      # concurrent suites never share a Zenoh session the way the DDS domain
      # separates the other legs. Multicast scouting stays off, as in the
      # distro default: otherwise every leased router joins one multicast
      # group and the suites merge no matter the port.
      router_config=$build/zenoh-router-$R2S_ZENOH_ROUTER_PORT.json5
      session_config=$build/zenoh-session-$R2S_ZENOH_ROUTER_PORT.json5
      printf '{\n  mode: "router",\n  scouting: { multicast: { enabled: false } },\n  listen: {\n    endpoints: ["tcp/127.0.0.1:%s"],\n  },\n}\n' "$R2S_ZENOH_ROUTER_PORT" >"$router_config"
      printf '{\n  scouting: { multicast: { enabled: false } },\n  connect: {\n    endpoints: ["tcp/127.0.0.1:%s"],\n  },\n}\n' "$R2S_ZENOH_ROUTER_PORT" >"$session_config"
      export ZENOH_ROUTER_CONFIG_URI=$router_config
      export ZENOH_SESSION_CONFIG_URI=$session_config
      "$router" >/dev/null 2>&1 &
      router=$!
      sleep 2
      kill -0 "$router" 2>/dev/null || { echo "rmw_zenohd on port $R2S_ZENOH_ROUTER_PORT died"; status=1; break; }
    fi
    for test_file in test_scenario_1_posix test_scenario_2_posix test_scenario_3_posix test_scenario_4_posix test_scenario_9_posix test_publish_catchup_posix; do
      timeout 300 launch_test "tests/e2e/${test_file}.py" || status=1
    done
    if [ -n "$router" ]; then kill "$router" 2>/dev/null || true; wait "$router" 2>/dev/null || true; fi
  done
done
exit $status
