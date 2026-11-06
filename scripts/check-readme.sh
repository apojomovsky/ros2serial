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
  "ports/posix/run.sh --build-only"
  "r2s_posix_firmware"
  "pty_pair.py"
  "ports/arduino/build.sh"
  "colcon build --base-paths bridge"
  "examples/arduino_minimal"
  "source build/bridge/install/setup.bash"
  "build/uno/arduino_minimal.ino.elf"
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

# The posix virtual MCU the walkthrough starts on. It lands under the same
# build directory, so the check and the README name the same firmware path.
bash ports/posix/run.sh --build-only || exit 1
posix_fw=$build/ports/posix/r2s_posix_firmware
[ -x "$posix_fw" ] || { echo "check-readme: no posix firmware at $posix_fw" >&2; exit 1; }

# The Arduino build the walkthrough runs: the helper stages the library
# and compiles the sketch, the way the README runs it. The library lands
# under a scratch directory in place of the README's default.
work=$build/walkthrough
lib=$work/lib
rm -rf "$work"
mkdir -p "$work" || exit 1
fw=build/uno
bash ports/arduino/build.sh --board uno --lib "$lib" --out "$fw" || exit 1
elf=$fw/arduino_minimal.ino.elf

# The harness the walkthrough runs, built by its own script, which stays
# the single description of how it builds.
R2S_BUILD_DIR=$PWD/build bash tests/harness/simavr/run.sh --build-only || exit 1
sim=build/harness/simavr/r2s_simavr
# The bridge build the walkthrough runs, from the repo root, exactly as
# documented: build outputs land under build/, which the checkout ignores.
colcon build --base-paths bridge --build-base build/bridge/build --install-base build/bridge/install || exit 1
# shellcheck source=/dev/null
set +u
source build/bridge/install/setup.bash
set -u

sim_pid=""
bridge_pid=""
posix_pids=""
cleanup() {
  [ -z "${bridge_pid:-}" ] || kill "$bridge_pid" 2>/dev/null || true
  [ -z "${sim_pid:-}" ] || kill "$sim_pid" 2>/dev/null || true
  # shellcheck disable=SC2086 # posix_pids holds several pids on purpose.
  [ -z "${posix_pids:-}" ] || kill $posix_pids 2>/dev/null || true
}
trap cleanup EXIT

# The emulated Uno the walkthrough starts on first.
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
echo "check-readme: Uno walkthrough is green"
[ -z "${bridge_pid:-}" ] || kill "$bridge_pid" 2>/dev/null || true
[ -z "${sim_pid:-}" ] || kill "$sim_pid" 2>/dev/null || true
bridge_pid=""
sim_pid=""

# The virtual serial line the walkthrough ties second: the relay prints the
# MCU and host ends, and the firmware and bridge each open one of them.
mkdir -p "$build/posix" || exit 1
python3 ports/posix/pty_pair.py >"$build/posix/ports.env" 2>"$build/posix/pty_pair.log" &
posix_pids="$posix_pids $!"
for _ in $(seq 1 20); do
  grep -q "^HOST_PORT=" "$build/posix/ports.env" 2>/dev/null && break
  sleep 0.25
done
grep -q "^MCU_PORT=" "$build/posix/ports.env" || { echo "check-readme: the relay announced no ports" >&2; exit 1; }
set -a
# shellcheck source=/dev/null
source "$build/posix/ports.env"
set +a

# The posix firmware the walkthrough starts, on the relay's MCU end.
"$posix_fw" "$MCU_PORT" >"$build/posix/firmware.log" 2>&1 &
posix_pids="$posix_pids $!"
for _ in $(seq 1 40); do
  grep -q "r2s_posix: ready" "$build/posix/firmware.log" 2>/dev/null && break
  sleep 0.25
done
grep -q "r2s_posix: ready" "$build/posix/firmware.log" || { echo "check-readme: the posix firmware never announced ready" >&2; exit 1; }

# The bridge the walkthrough launches on the relay's host end.
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
ros2 launch r2s_bridge r2s_bridge.launch.py port:="$HOST_PORT" >"$build/posix/bridge.log" 2>&1 &
posix_pids="$posix_pids $!"
seen=""
for _ in $(seq 1 60); do
  seen=$(ros2 topic list 2>/dev/null || true)
  case "$seen" in *"/battery"*) break ;; *) seen="" ;; esac
  sleep 1
done
[ -n "$seen" ] || { echo "check-readme: posix /battery never appeared" >&2; exit 1; }
for topic in /battery /cmd_vel /diagnostics /parameter_events /rosout; do
  echo "$seen" | grep -qx "$topic" || { echo "check-readme: posix $topic never appeared" >&2; exit 1; }
done
echo "check-readme: posix topics announced"
timeout 60 ros2 topic echo /battery std_msgs/msg/Float32 --once >"$build/posix/echo.txt" 2>&1 || {
  echo "check-readme: no posix battery reading arrived" >&2
  exit 1
}
grep -q "data:" "$build/posix/echo.txt" || { echo "check-readme: the posix echo held no reading" >&2; exit 1; }
cat "$build/posix/echo.txt"
echo "check-readme: posix walkthrough is green"
# shellcheck disable=SC2086 # posix_pids holds several pids on purpose.
kill $posix_pids 2>/dev/null || true
posix_pids=""
echo "check-readme: walkthrough is green"
