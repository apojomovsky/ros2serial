#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The Arduino IDE custom-message walkthrough, run as the page prints it.
#
# The page's file blocks become the reader's interface package and sketch, its
# run blocks build the package, generate the archive and compile the sketch
# against it, and its serve and live blocks are replayed against an emulated
# Uno with the bridge. A command that drifts from the page fails here, naming
# the block's line, so the walkthrough cannot rot while the code moves.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2

if [ -z "${R2S_WALKTHROUGH_LEASED:-}" ]; then
  exec env R2S_WALKTHROUGH_LEASED=1 bash "$root/scripts/e2e-lease.sh" \
    bash "$root/scripts/check-walkthrough-arduino-ide.sh" "$@"
fi
[ "$#" -eq 0 ] || { echo "usage: scripts/check-walkthrough-arduino-ide.sh" >&2; exit 2; }

page=docs/arduino-ide-custom-messages.md
work=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/walkthrough

# One lock for the work directory, held for the whole run: a direct run and a
# suite run of one tree must not write the same firmware. The lock file sits
# beside the directory, not inside it, because the run clears the directory and
# unlinking a lock file lets the next run lock a fresh inode while this one
# still holds the old.
if [ -z "${R2S_WALKTHROUGH_LOCKED:-}" ]; then
  mkdir -p "$(dirname "$work")" || exit 1
  exec env R2S_WALKTHROUGH_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$work.lock" \
    bash "$root/scripts/check-walkthrough-arduino-ide.sh"
fi
rm -rf "$work"
mkdir -p "$work" || exit 1

: "${ROS_DISTRO:?ROS_DISTRO is not set: run it in a ROS container}"
if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/$ROS_DISTRO/setup.bash" ]; then
  # shellcheck source=/dev/null
  set +u
  source "/opt/ros/$ROS_DISTRO/setup.bash"
  set -u
fi

# The parser runs before anything is built: a page whose marker names no
# fence, or a block whose fence is not shell, fails without spending a build.
python3 tools/docs_walkthrough.py --page "$page" || exit 1

# The bridge and the generator the page reaches through `ros2 run`, installed
# as the reader installs them.
colcon build --base-paths bridge gen gen/r2s_rosidl \
  --packages-up-to r2s_bridge r2s_gen r2s_rosidl \
  --build-base "$work/install/build" --install-base "$work/install/root" \
  --cmake-args -DBUILD_TESTING=OFF || exit 1
# shellcheck source=/dev/null
set +u
source "$work/install/root/setup.bash"
set -u
ros2 run r2s_gen r2s-gen version || exit 1

# The page's files, then its commands, in the order it prints them: the
# interface package builds, the archive is generated, and the sketch compiles
# against the archive the IDE would install.
page_home=$work/home
python3 tools/docs_walkthrough.py --page "$page" --run --home "$page_home" || exit 1

# The emulated Uno the page's last two blocks run against. The harness builds
# through its own script, which stays the single description of how it builds.
R2S_BUILD_DIR=$work/harness bash tests/harness/simavr/run.sh --build-only || exit 1
sim=$work/harness/harness/simavr/r2s_simavr
elf=$page_home/firmware/build/sensor_sketch.ino.elf
[ -x "$sim" ] || { echo "check-walkthrough: no harness at $sim" >&2; exit 1; }
[ -f "$elf" ] || { echo "check-walkthrough: the page compiled no firmware at $elf" >&2; exit 1; }

"$sim" --mcu atmega328p --firmware "$elf" --run-ms 300000 >"$work/simavr.log" 2>&1 &
sim_pid=$!
bridge_pid=""
killtree() { bash "$root/scripts/kill-tree.sh" "$1"; }
cleanup() {
  [ -z "${bridge_pid:-}" ] || killtree "$bridge_pid"
  [ -z "${sim_pid:-}" ] || killtree "$sim_pid"
}
trap cleanup EXIT

pty=""
for _ in $(seq 1 40); do
  pty=$(grep -m1 "r2s_simavr: pty=" "$work/simavr.log" 2>/dev/null || true)
  pty=${pty##*=}
  case "$pty" in /dev/*) break ;; *) pty="" ;; esac
  sleep 0.5
done
[ -n "$pty" ] || { echo "check-walkthrough: the harness announced no pty" >&2; exit 1; }
echo "check-walkthrough: firmware on $pty"

# The serve and live blocks, with the port the walkthrough's own block names
# replaced by the emulated one: the rest of each command runs as printed.
emitted=$work/emitted
python3 tools/docs_walkthrough.py --page "$page" --emit "$emitted" --port "$pty" || exit 1
[ -f "$emitted/serve.sh" ] && [ -f "$emitted/live.sh" ] || {
  echo "check-walkthrough: the page names no serve and live block pair" >&2
  exit 1
}

# The package the page built on the host, so the bridge can load the message
# type the firmware announces.
# shellcheck source=/dev/null
set +u
source "$page_home/ros2_ws/install/setup.bash"
set -u

export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
bash "$emitted/serve.sh" >"$work/bridge.log" 2>&1 &
bridge_pid=$!
# One reading, with the bounded retry the README walkthrough uses, since the
# board's first frames race the bridge's graph.
bash scripts/with-retry.sh --tries 20 --timeout 20 --out "$work/echo.txt" -- \
  bash "$emitted/live.sh" || {
  echo "check-walkthrough: the live block read no reading" >&2
  tail -40 "$work/bridge.log" >&2
  exit 1
}
# Both fields travel in the same payload, so a reading carrying them proves the
# layout survived the whole path.
grep -q "^temperature:" "$work/echo.txt" || { echo "check-walkthrough: no temperature" >&2; exit 1; }
grep -q "^status:" "$work/echo.txt" || { echo "check-walkthrough: no status" >&2; exit 1; }
cat "$work/echo.txt"
echo "check-walkthrough: the Arduino IDE custom-message walkthrough is green"
