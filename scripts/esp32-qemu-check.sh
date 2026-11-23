#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The ESP32 emulation spike's entry point: build arduino_button_led for the
# ESP32 Dev Module, merge its four flash parts into one 4 MB image, run it
# under Espressif's QEMU against the bridge, and pass when the bridge
# reports connected and at least 20 /button messages arrive within 60 s.
# Rates and gaps stay out: emulation time is not wall time. Usage, from the
# repository root: bash scripts/esp32-qemu-check.sh
# The host path builds docker/esp32-qemu and runs this script again inside
# it, where qemu-system-xtensa is on PATH; the container shares the
# repository and the cache with the host, so the firmware, the bridge and
# the flash image land where make targets keep them. The root resolves with
# builtins only, so the missing-emulator refusal needs no PATH at all.
set -uo pipefail
script_dir=${BASH_SOURCE[0]%/*}
root=$(cd "$script_dir/.." && pwd)
cd "$root" || exit 2
[ "$#" -eq 0 ] || {
  echo "usage: bash scripts/esp32-qemu-check.sh" >&2
  exit 2
}

IMAGE=ros2serial-esp32-qemu:local
SKETCH=arduino_button_led
WANT_BUTTONS=20
BUDGET_S=60
# The Dev Module flash QEMU maps as one mtd drive: a short merged image is
# padded out to it.
FLASH_BYTES=4194304
# DDS discovery is host-wide, so the spike takes its own domain: the e2e
# pool starts at 89, and another agent's run must never lend us buttons.
DOMAIN_ID=${R2S_QEMU_DOMAIN_ID:-72}

pin() { sed -n "s/^$1=//p" "$root/ci/versions.env" | tail -1; }
say() { echo "esp32-qemu-check: $*" >&2; }

if [ -z "${R2S_IN_IMAGE:-}" ]; then
  command -v docker >/dev/null 2>&1 || {
    echo "esp32-qemu-check: docker not found: the host path builds docker/esp32-qemu" >&2
    exit 1
  }
  dev=$(bash "$root/scripts/dev-image-tag.sh")
  docker build -f "$root/docker/esp32-qemu/Dockerfile" -t "$IMAGE" \
    --build-arg "DEV_IMAGE=$dev" \
    --build-arg "ESP_QEMU_VERSION=$(pin ESP_QEMU_VERSION)" \
    --build-arg "ESP_QEMU_SHA256=$(pin ESP_QEMU_SHA256)" \
    "$root" || exit 1
  cache=${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}
  build=${R2S_BUILD_DIR:-$cache/build$(echo "$root" | tr / -)}
  mkdir -p "$cache" "$build" || exit 1
  exec docker run --rm --user "$(id -u):$(id -g)" \
    -v "$root:$root" -w "$root" -v "$cache:$cache" \
    -e "R2S_BUILD_DIR=$build" -e "R2S_CACHE_DIR=$cache" -e "R2S_HOST_MOUNT=$root" \
    -e "R2S_QEMU_DOMAIN_ID=$DOMAIN_ID" \
    "$IMAGE" bash scripts/esp32-qemu-check.sh
fi

command -v qemu-system-xtensa >/dev/null 2>&1 || {
  echo "esp32-qemu-check: no qemu-system-xtensa on PATH:" >&2
  echo "run bash scripts/esp32-qemu-check.sh on the host, which builds docker/esp32-qemu" >&2
  exit 1
}

cache=${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}
build=${R2S_BUILD_DIR:-$cache/build$(echo "$root" | tr / -)}
out=$build/esp32-qemu
mkdir -p "$out" || exit 1

say "building the sketches for the ESP32 Dev Module"
bash scripts/build-examples-esp32.sh || exit 1
# The build stages every sketch under an examples level beside its own
# build directory, so button_led's firmware lives two levels down.
merged=$build/examples/arduino_button_led/esp32/arduino_button_led.ino.merged.bin
[ -f "$merged" ] || {
  echo "esp32-qemu-check: the ESP32 build left no $merged" >&2
  exit 1
}

flash=$out/flash.bin
say "taking the core's merged flash image"
cp "$merged" "$flash" || exit 1
size=$(stat -c %s "$flash")
if [ "$size" -gt "$FLASH_BYTES" ]; then
  echo "esp32-qemu-check: the merged image is $size bytes, over $FLASH_BYTES" >&2
  exit 1
fi
# QEMU maps the whole 4 MB flash as one mtd drive, so a short image is
# padded; truncate only grows, and the check above keeps it from shrinking.
truncate -s "$FLASH_BYTES" "$flash"

say "building the bridge"
bash scripts/bridge-test.sh --build-only || exit 1
install=$build/bridge/install
[ -f "$install/setup.bash" ] || {
  echo "esp32-qemu-check: no bridge install at $install: the bridge build did not finish" >&2
  exit 1
}

# Both ROS sides share the spike's domain, so no other run lends us topics.
export ROS_DOMAIN_ID="$DOMAIN_ID"

# TERM reaches the launcher and the node it started; the pty dies with QEMU
# itself, so no launch left holding it can fail the next run.
stop() {
  kill -TERM "$1" 2>/dev/null || true
  for _ in $(seq 1 50); do
    kill -0 "$1" 2>/dev/null || return 0
    sleep 0.1
  done
  kill -KILL "$1" 2>/dev/null || true
}
cleanup() {
  [ -z "${bridge_pid:-}" ] || stop "$bridge_pid"
  [ -z "${qemu_pid:-}" ] || stop "$qemu_pid"
  wait 2>/dev/null || true
}
trap cleanup EXIT

qemu_log=$out/qemu.log
rm -f "$qemu_log"
say "starting QEMU on a pty"
qemu-system-xtensa -M esp32 -nographic -drive "file=$flash,if=mtd,format=raw" -serial pty \
  >"$qemu_log" 2>&1 &
qemu_pid=$!
port=""
for _ in $(seq 1 150); do
  if ! kill -0 "$qemu_pid" 2>/dev/null; then
    echo "esp32-qemu-check: QEMU exited during startup; its log ends:" >&2
    tail -n 10 "$qemu_log" >&2 || true
    exit 1
  fi
  # QEMU announces its slave as: char device redirected to /dev/pts/N ...
  line=$(grep -m 1 'char device redirected to ' "$qemu_log" 2>/dev/null || true)
  if [ -n "$line" ]; then
    port=$(echo "$line" | sed -n 's/.*char device redirected to \([^ ]*\).*/\1/p')
    [ -n "$port" ] && break
  fi
  sleep 0.1
done
[ -n "$port" ] || {
  echo "esp32-qemu-check: QEMU named no pty within 15 s; its log ends:" >&2
  tail -n 10 "$qemu_log" >&2 || true
  exit 1
}
say "QEMU serves $port"

# The launch joins from its install the way the hardware check launches it:
# the node's own output stays in the bridge log, and the probe counts from
# ROS topics, never by touching the port itself.
bridge_log=$out/bridge.log
inner=$(
  cat <<'EOF'
if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
  source "/opt/ros/${ROS_DISTRO}/setup.bash"
fi
source "$1"/setup.bash && exec ros2 launch r2s_bridge r2s_bridge.launch.py port:="$2"
EOF
)
bash -c "$inner" r2s-bridge "$install" "$port" >"$bridge_log" 2>&1 &
bridge_pid=$!
say "starting the bridge on $port (log: $bridge_log)"

probe_log=$out/probe.log
probe_inner=$(
  cat <<'EOF'
if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
  source "/opt/ros/${ROS_DISTRO}/setup.bash"
fi
source "$1"/setup.bash && exec python3 tools/esp32_qemu_probe.py --want "$2" --budget "$3"
EOF
)
# The probe counts from here: connected plus WANT_BUTTONS arrivals inside
# BUDGET_S seconds is the ticket's pass line.
probe_start=$(date +%s)
if bash -c "$probe_inner" r2s-probe "$install" "$WANT_BUTTONS" "$BUDGET_S" >"$probe_log" 2>&1; then
  status=0
else
  status=1
fi
probe_end=$(date +%s)
cat "$probe_log"
say "qemu $(qemu-system-xtensa --version | head -n 1)"
say "run took $((probe_end - probe_start)) s wall time"
exit "$status"
