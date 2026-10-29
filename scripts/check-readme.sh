#!/usr/bin/env bash
# The README walkthrough check: every Getting started command runs here on
# simavr, so the walkthrough cannot drift from the code.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2

# One isolation slot per run: a leased DDS domain, so parallel runs on the
# shared host never see each other's topics. Held for the whole check.
if [ -z "${R2S_README_LEASED:-}" ]; then
  exec env R2S_README_LEASED=1 bash "$root/scripts/e2e-lease.sh" bash "$root/scripts/check-readme.sh" "$@"
fi
[ "$#" -eq 0 ] || { echo "usage: scripts/check-readme.sh" >&2; exit 2; }

# The walkthrough commands below stay quoted in the README's Getting started
# section. A renamed command fails here, naming the missing line.
anchors=(
  "## Getting started"
  "pip install ./gen"
  "arduino-cli core install arduino:avr"
  "r2s-gen bundle --distro jazzy"
  "ports/arduino/r2s_arduino_port"
  "ports/arduino/r2s_arduino_port.cpp"
  "cpp/src/runtime.cpp"
  "colcon build --base-paths bridge"
  "examples/arduino_minimal"
  "tests/harness/simavr/run.sh --build-only"
  "r2s_simavr --mcu atmega328p"
  "r2s_bridge.launch.py port:="
  "ros2 topic echo /battery std_msgs/msg/Float32 --once"
  "arduino-cli upload"
  "/dev/ttyACM0"
  "boot_wait_ms"
)
for anchor in "${anchors[@]}"; do
  grep -qF "$anchor" README.md || { echo "check-readme: README.md lacks '$anchor'" >&2; exit 1; }
done
echo "check-readme: every walkthrough command is present in README.md"

command -v colcon >/dev/null 2>&1 || { echo "colcon not found: run it in a ROS container"; exit 1; }
: "${ROS_DISTRO:?ROS_DISTRO is not set: run it in a ROS container}"
if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/$ROS_DISTRO/setup.bash" ]; then
  # shellcheck source=/dev/null
  set +u
  source "/opt/ros/$ROS_DISTRO/setup.bash"
  set -u
fi
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/readme
mkdir -p "$build" || exit 1
export R2S_BUILD_DIR=$build

# The generator install the walkthrough starts from.
python3 -m pip install --quiet ./gen || exit 1
case ":$PATH:" in
  *":$HOME/.local/bin:"*) ;;
  *) export PATH="$HOME/.local/bin:$PATH" ;;
esac
r2s-gen version || exit 1

# The AVR core the walkthrough installs. The dev image carries the pinned
# core, so a present core stays untouched and no network is needed.
if arduino-cli core list | grep -q "arduino:avr"; then
  echo "check-readme: arduino:avr core already installed"
else
  arduino-cli core install arduino:avr || exit 1
fi

# The Arduino library the walkthrough assembles: the bundle plus the core,
# the port glue and the runtime layer, under a scratch directory in place
# of the README's sketchbook path.
work=$build/walkthrough
lib=$work/lib
rm -rf "$work"
mkdir -p "$lib/src/r2s" || exit 1
r2s-gen bundle --distro jazzy --out "$lib" || exit 1
cp core/include/r2s/*.h "$lib/src/r2s/" || exit 1
cp core/src/*.c "$lib/src/" || exit 1
rm -f "$lib/src/test_reset.c"
cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$lib/src/" || exit 1
cp cpp/include/r2s/runtime.hpp "$lib/src/r2s/" || exit 1
cp cpp/src/runtime.cpp "$lib/src/" || exit 1

# The compile the walkthrough runs: the sketch's frame and nesting limits
# travel as flags, since the core never includes the bundled header.
extras=$(python3 - "$lib/src/r2s_bundled.h" <<'EOF'
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
fw=$work/fw
arduino-cli compile --fqbn arduino:avr:uno \
  --build-property "compiler.c.extra_flags=$extras" \
  --build-property "compiler.cpp.extra_flags=$extras" \
  --library "$lib" --output-dir "$fw" examples/arduino_minimal || exit 1
elf=$fw/arduino_minimal.ino.elf

# The harness the walkthrough runs, built by its own script, which stays
# the single description of how it builds.
bash tests/harness/simavr/run.sh --build-only || exit 1
sim=$build/harness/simavr/r2s_simavr
# The bridge build the walkthrough runs: the same colcon line, with the
# bases relocated under the build directory so the tree stays clean.
colcon build --base-paths bridge --build-base "$build/bridge/build" --install-base "$build/bridge/install" || exit 1
# shellcheck source=/dev/null
set +u
source "$build/bridge/install/setup.bash"
set -u

sim_pid=""
bridge_pid=""
cleanup() {
  [ -z "${bridge_pid:-}" ] || kill "$bridge_pid" 2>/dev/null || true
  [ -z "${sim_pid:-}" ] || kill "$sim_pid" 2>/dev/null || true
}
trap cleanup EXIT

# The emulated Uno the walkthrough runs instead of hardware.
"$sim" --mcu atmega328p --firmware "$elf" --run-ms 300000 >"$work/simavr.log" 2>&1 &
sim_pid=$!
pty=""
for _ in $(seq 1 40); do
  pty=$(grep -m1 "r2s_simavr: pty=" "$work/simavr.log" 2>/dev/null || true)
  pty=${pty##*=}
  case "$pty" in /dev/*) break ;; *) pty="" ;; esac
  sleep 0.5
done
[ -n "$pty" ] || { echo "check-readme: the harness announced no pty" >&2; exit 1; }
echo "check-readme: firmware on $pty"

# The bridge the walkthrough launches, on the emulated port.
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
ros2 launch r2s_bridge r2s_bridge.launch.py port:="$pty" >"$work/bridge.log" 2>&1 &
bridge_pid=$!
seen=""
for _ in $(seq 1 60); do
  seen=$(ros2 topic list 2>/dev/null || true)
  case "$seen" in *"/battery"*) break ;; *) seen="" ;; esac
  sleep 1
done
[ -n "$seen" ] || { echo "check-readme: /battery never appeared" >&2; exit 1; }
# The topics the walkthrough shows: the firmware pair plus the node's own.
for topic in /battery /cmd_vel /diagnostics /parameter_events /rosout; do
  echo "$seen" | grep -qx "$topic" || { echo "check-readme: $topic never appeared" >&2; exit 1; }
done
echo "check-readme: topics announced"
echo "$seen"

# The echo the walkthrough ends on: one battery reading from the Uno.
timeout 60 ros2 topic echo /battery std_msgs/msg/Float32 --once >"$work/echo.txt" 2>&1 || {
  echo "check-readme: no battery reading arrived" >&2
  exit 1
}
grep -q "data:" "$work/echo.txt" || { echo "check-readme: the echo held no reading" >&2; exit 1; }
cat "$work/echo.txt"
echo "check-readme: walkthrough is green"
