# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Sourced, not run: installs the ESP32 Arduino core at the version pinned in
# ci/versions.env into the cache data directory, and puts the ESP32 toolchain
# bin directories on PATH. Both ports/arduino/build.sh (for the ESP32 boards
# of ports/arduino/boards.list) and scripts/build-examples-esp32.sh source
# this file, so the core setup stays in one place. A failure exits the caller.
# The data directory defaults under R2S_CACHE_DIR, which the image mounts on
# the shared cache, because the image's own HOME is ephemeral.
esp32_url="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
esp32_dir=${R2S_ARDUINO_DATA_DIR:-${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}/arduino-esp32}
esp32_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
esp32_pin=$(grep '^ARDUINO_ESP32_CORE_VERSION=' "$esp32_root/ci/versions.env" | cut -d= -f2)
[ -n "$esp32_pin" ] || {
  echo "esp32-core: ci/versions.env names no ARDUINO_ESP32_CORE_VERSION" >&2
  exit 1
}
export ARDUINO_DIRECTORIES_DATA="$esp32_dir"
export ARDUINO_DIRECTORIES_USER="$esp32_dir/user"
mkdir -p "$esp32_dir/user" || exit 1
if ! arduino-cli core list 2>/dev/null | grep -q "esp32:esp32 $esp32_pin"; then
  arduino-cli core update-index --additional-urls "$esp32_url" || exit 1
  arduino-cli core install --additional-urls "$esp32_url" "esp32:esp32@$esp32_pin" || exit 1
fi
# Each ESP32 board of the list names its toolchain's binutils prefix, so the
# setup searches for <prefix>-size instead of naming the versioned folder the
# core ships the toolchain in; the pin fixes the layout the search walks. One
# bin directory can hold several prefixes, so each joins PATH once.
# shellcheck disable=SC2207 # splitting the prefix list is the point.
esp32_prefixes=($(awk 'NF && $1 !~ /^#/ && $2 ~ /^esp32:/ {print $3}' \
  "$esp32_root/ports/arduino/boards.list" | sort -u))
[ "${#esp32_prefixes[@]}" -gt 0 ] || {
  echo "esp32-core: ports/arduino/boards.list names no ESP32 board" >&2
  exit 1
}
esp32_path="$PATH"
for esp32_prefix in "${esp32_prefixes[@]}"; do
  esp32_size=$(find "$esp32_dir/packages/esp32/tools" -name "$esp32_prefix-size" -type f 2>/dev/null | head -n 1)
  [ -n "$esp32_size" ] || {
    echo "esp32-core: no $esp32_prefix toolchain under $esp32_dir" >&2
    exit 1
  }
  esp32_bin=$(dirname "$esp32_size")
  case "$esp32_path" in
    "$esp32_bin" | "$esp32_bin":* | *":$esp32_bin" | *":$esp32_bin:"*) ;;
    *) esp32_path="$esp32_bin:$esp32_path" ;;
  esac
done
export PATH="$esp32_path"
unset esp32_url esp32_dir esp32_root esp32_pin esp32_prefixes esp32_path esp32_prefix esp32_size esp32_bin
