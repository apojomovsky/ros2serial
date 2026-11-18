#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The make hardware-check plumbing: hand the serial device to the dev image
# with the group that owns it, so the caller's uid need not be in dialout,
# then run the check inside. Usage: hardware-check.sh <port> <board>.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if [ "$#" -ne 2 ]; then
  echo "usage: make hardware-check PORT=/dev/ttyACM0 BOARD=uno|mega" >&2
  exit 2
fi
port=$1
board=$2
case "$board" in
  uno | mega) ;;
  *)
    echo "make hardware-check: BOARD must be uno or mega, got '$board'" >&2
    exit 2
    ;;
esac
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
