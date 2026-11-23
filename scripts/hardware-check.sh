#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The make hardware-check plumbing: hand the serial device to the dev image
# with the group that owns it, so the caller's uid need not be in dialout,
# then run the check inside. Usage: hardware-check.sh <port> <board>. The
# board names come from ports/arduino/boards.list, and the transport check
# below runs on the host, where /sys is the host's.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
boards_list="$root/ports/arduino/boards.list"

board_names() {
  awk 'NF && $1 !~ /^#/ {print $1}' "$boards_list"
}

board_field() {
  awk -v board="$1" -v column="$2" \
    'NF && $1 !~ /^#/ && $1 == board {print $column}' "$boards_list"
}

names_pipe=$(board_names | paste -sd'|')
names_space=$(board_names | paste -sd' ')

if [ "$#" -ne 2 ]; then
  echo "usage: make hardware-check PORT=/dev/ttyACM0 BOARD=$names_pipe" >&2
  exit 2
fi
port=$1
board=$2
fqbn=$(board_field "$board" 2)
if [ -z "$fqbn" ]; then
  echo "make hardware-check: BOARD must be one of $names_space, got '$board'" >&2
  exit 2
fi
if [ -z "$port" ]; then
  echo "make hardware-check: PORT is required, for example PORT=/dev/ttyACM0" >&2
  echo "take the port from: arduino-cli board list" >&2
  exit 2
fi
# A by-id symlink names the board, but the container gets the device node it
# points at, so every path below is the resolved one.
if [ -L "$port" ]; then
  resolved=$(readlink -f "$port")
  [ -n "$resolved" ] && port=$resolved
fi
if [ ! -e "$port" ]; then
  echo "make hardware-check: no device at $port" >&2
  echo "plug the board in, then take its port from: arduino-cli board list" >&2
  exit 2
fi

# The ESP32 transport check: a -usb board talks native USB (Espressif VID
# 303a), while the other ESP32 boards talk through the COM/UART bridge, so a
# VID that matches the wrong side means the cable is on the wrong port. AVR
# boards skip the check. An unreadable sysfs skips it too.
case "$fqbn" in
  esp32:*)
    dev=$(basename "$port")
    sysfs=${R2S_SYSFS_ROOT:-/sys}
    vendor_file="$sysfs/class/tty/$dev/device/../idVendor"
    if [ -r "$vendor_file" ]; then
      vendor=$(tr '[:upper:]' '[:lower:]' <"$vendor_file" | tr -d ' \t\n')
      case "$board" in
        *-usb)
          if [ "$vendor" != "303a" ]; then
            echo "make hardware-check: BOARD $board needs the USB port, but $port" >&2
            echo "has vendor ${vendor:-unknown}: unplug from the COM/UART port" >&2
            echo "and use the USB one, then rerun" >&2
            exit 2
          fi
          ;;
        *)
          if [ "$vendor" = "303a" ]; then
            echo "make hardware-check: BOARD $board needs the COM/UART port, but $port" >&2
            echo "looks like the native USB one (vendor 303a): unplug from USB" >&2
            echo "and use the COM/UART port, then rerun" >&2
            exit 2
          fi
          ;;
      esac
    fi
    ;;
esac

if [ "${R2S_IN_IMAGE:-}" = 1 ]; then
  exec python3 "$root/tools/hardware_check.py" --port "$port" --board "$board"
fi

# The device's group, so the container's uid opens it as a member.
group=$(stat -c %g "$port")
cache=${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}
build=${R2S_BUILD_DIR:-$cache/build$(echo "$root" | tr / -)}
image=$(bash "$root/scripts/dev-image-tag.sh")
if ! docker image inspect "$image" >/dev/null 2>&1; then
  echo "make hardware-check: the dev image $image is not built" >&2
  echo "run: make image" >&2
  exit 2
fi
# The LED question reads the caller's terminal, so stdin stays attached and a
# tty is asked for only when there is one.
tty_flag=""
[ -t 0 ] && tty_flag="-t"
mkdir -p "$cache" || exit 1
read -r -a pass_vars < <(grep -v '^#' "$root/scripts/pass-vars.list" | tr '\n' ' ')
env_args=()
for name in "${pass_vars[@]}"; do
  [ -n "${!name:-}" ] && env_args+=("-e" "$name")
done
exec docker run --rm -i $tty_flag --user "$(id -u):$(id -g)" \
  --device "$port" --group-add "$group" \
  -v "$root:$root" -w "$root" \
  -v "$cache:$cache" \
  -e "R2S_BUILD_DIR=$build" -e "R2S_CACHE_DIR=$cache" -e "R2S_HOST_MOUNT=$root" \
  "${env_args[@]}" \
  "$image" python3 "$root/tools/hardware_check.py" --port "$port" --board "$board"
