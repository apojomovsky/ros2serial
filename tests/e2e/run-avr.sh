#!/usr/bin/env bash
# The avr end-to-end suite: scenarios 1 to 4 against the slice sketch on an
# emulated Uno and Mega. It stages the Arduino library at build time (the
# bundle plus the core and the port glue, as scripts/build-examples.sh does),
# compiles the sketch for both boards with burst-sized serial rings, and runs
# every scenario under each RMW the design names, starting the Zenoh router
# first for its leg. Builds go under R2S_BUILD_DIR, so the tree stays clean;
# the suite runs this script for E2E=avr. R2S_RMWS narrows the RMW loop and
# R2S_E2E_BOARDS the board loop for a quicker iteration.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/e2e/avr
# One lock for the e2e build directory, held for the entire run: a direct run
# and a suite run of one tree must not write the same binaries.
if [ -z "${R2S_E2E_AVR_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_E2E_AVR_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/tests/e2e/run-avr.sh" "$@"
fi
mkdir -p "$build" || exit 1

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: tests/e2e/run-avr.sh [--build-only]" >&2; exit 2; }

# The harness binary the scenarios spawn: the simavr script builds it with
# --build-only and skips its own tests, which the suite runs separately.
bash tests/harness/simavr/run.sh --build-only || exit 1
sim_build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/harness/simavr
export R2S_SIMAVR_BIN=$sim_build/r2s_simavr

# The Arduino library is staged at build time: the bundle carries the sketch
# list's descriptors and the port glue, the core joins it here, so no bytes
# live twice in the repository and nothing can drift.
stage="$build/avr_slice"
rm -rf "$stage"
mkdir -p "$stage/src/r2s" || exit 1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli bundle --distro jazzy --out "$stage" || exit 1
cp core/include/r2s/*.h "$stage/src/r2s/" || exit 1
cp core/src/*.c "$stage/src/" || exit 1
rm -f "$stage/src/test_reset.c"
cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$stage/src/" || exit 1
# The core never includes the bundled header, so the limits travel as -D
# flags read from it: the same set the cmake layout passes as definitions.
# A library sets none of its own flags on this toolchain, hence --build-property.
extras=$(PYTHONPATH="$root/gen" python3 - "$stage/src/r2s_bundled.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "src/r2s_bundled.h")
except (MessageError, OSError) as error:
    print(f"r2s-gen: error: {error}", file=sys.stderr)
    raise SystemExit(1)
print(" ".join("-D" + define for define in defines))
EOF
) || exit 1
# A flag string without the limits would compile a firmware that silently
# ignores every subscription or refuses its own table, so every value must
# be present and non-empty.
case "$extras" in
    *R2S_WITH_CODEC*R2S_MAX_FRAME=[0-9]*R2S_MAX_NEST=[0-9]*) ;;
    *)
        echo "run-avr: the extracted flags name no limits ($extras)" >&2
        exit 1
        ;;
esac
# Burst-sized rings (R10): the RX ring covers the framer capacity for a
# maximum frame, the TX ring a whole wire frame, so neither direction drops
# or stalls on a burst the slice can send.
serial_flags="-DSERIAL_RX_BUFFER_SIZE=256 -DSERIAL_TX_BUFFER_SIZE=256"
mkdir -p "$build/fw" || exit 1
for board in uno mega; do
  mcu=atmega328p
  [ "$board" = mega ] && mcu=atmega2560
  out="$build/fw/$board"
  arduino-cli compile --fqbn "arduino:avr:$board" \
    --build-property "compiler.c.extra_flags=$extras" \
    --build-property "compiler.cpp.extra_flags=$serial_flags" \
    --library "$stage" --output-dir "$out" tests/e2e/avr_slice || exit 1
  avr-size "$out/avr_slice.ino.elf" || exit 1
  mv "$out/avr_slice.ino.elf" "$build/fw/slice-$mcu.elf" || exit 1
done
export R2S_E2E_AVR_FW=$build/fw
[ "$build_only" = 1 ] && exit 0

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

# The scenario files import the framework as a package; direct launch_test
# runs need the same directory on the import path.
export PYTHONPATH=$root/tests/e2e${PYTHONPATH:+:$PYTHONPATH}
# The helper assertions run before any scenario, so a broken helper fails
# fast instead of mid-handshake; the script directory is already on the
# import path when Python runs the file directly.
python3 tests/e2e/checks_test.py || exit 1
python3 tests/e2e/test_avr_support.py || exit 1

# One DDS domain per suite run, so parallel runs on the shared host never see
# each other's topics: every process below inherits the same domain.
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:-89}

boards=${R2S_E2E_BOARDS:-"uno mega"}
rmws=${R2S_RMWS:-rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_zenoh_cpp}

status=0
for rmw in $rmws; do
  echo "== e2e-avr: $rmw"
  export RMW_IMPLEMENTATION=$rmw
  router=""
  if [ "$rmw" = rmw_zenoh_cpp ]; then
    router=$(find "/opt/ros/$ROS_DISTRO" -name rmw_zenohd -type f 2>/dev/null | head -n 1)
    [ -n "$router" ] || { echo "rmw_zenohd not found for $ROS_DISTRO"; status=1; break; }
    "$router" >/dev/null 2>&1 &
    router=$!
    sleep 2
  fi
  for board in $boards; do
    export R2S_E2E_BOARD=$board
    for scenario in 1 2 3 4; do
      timeout 300 launch_test "tests/e2e/test_scenario_${scenario}_avr.py" || status=1
    done
  done
  if [ -n "$router" ]; then kill "$router" 2>/dev/null || true; wait "$router" 2>/dev/null || true; fi
done
exit $status
