#!/usr/bin/env bash
# Build every example sketch for the ESP32 Dev Module through the Arduino
# layout: the ESP32 core installs into a cache directory at the version
# pinned in ci/versions.env, never into the dev image. The staged library is
# the same bundle the AVR build stages, so the two builds share no sources.
# Builds go under R2S_BUILD_DIR, so the tree stays clean.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/examples
# One lock for the examples build directory, held for the whole run: a direct
# run and a suite run of one tree must not write the same ELF at once.
if [ -z "${R2S_EXAMPLES_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_EXAMPLES_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/scripts/build-examples-esp32.sh" "$@"
fi
mkdir -p "$build" || exit 1

# The ESP32 core lives outside the image: the first run downloads it into the
# cache, later runs and CI restore it from the cache. The data directory
# defaults under R2S_CACHE_DIR, which the image mounts on the shared cache,
# because the image's own HOME is ephemeral.
esp32_url="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
data_dir=${R2S_ARDUINO_DATA_DIR:-${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}/arduino-esp32}
pin=$(grep '^ARDUINO_ESP32_CORE_VERSION=' "$root/ci/versions.env" | cut -d= -f2)
[ -n "$pin" ] || {
  echo "build-examples-esp32: ci/versions.env names no ARDUINO_ESP32_CORE_VERSION" >&2
  exit 1
}
export ARDUINO_DIRECTORIES_DATA="$data_dir"
export ARDUINO_DIRECTORIES_USER="$data_dir/user"
mkdir -p "$data_dir/user" || exit 1
if ! arduino-cli core list 2>/dev/null | grep -q "esp32:esp32 $pin"; then
  arduino-cli core update-index --additional-urls "$esp32_url" || exit 1
  arduino-cli core install --additional-urls "$esp32_url" "esp32:esp32@$pin" || exit 1
fi
# The Xtensa binutils measure the firmware for the budgets table, which runs
# later in the same shell, so the toolchain joins PATH here. The folder name
# holding the toolchain moves between core releases, so the script locates
# the size binary instead of naming the folder; the pin fixes the layout.
tc_size=$(find "$data_dir/packages/esp32/tools" -name xtensa-esp32-elf-size -type f 2>/dev/null | head -n 1)
[ -n "$tc_size" ] || {
  echo "build-examples-esp32: no xtensa toolchain under $data_dir" >&2
  exit 1
}
export PATH="$(dirname "$tc_size"):$PATH"

# The runtime sketch builds against the bundled library: r2s-gen emits the
# library at build time, the core and the C++ runtime layer join it here,
# and no r2s.yaml is involved. The sketch registers its topics while it
# runs, so the library carries the runtime API and the message headers
# instead of a generated topic table.
stage="$build/esp32-stage"
rm -rf "$stage"
mkdir -p "$stage/src/r2s" || exit 1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli bundle --distro jazzy --out "$stage" || exit 1
cp core/include/r2s/*.h "$stage/src/r2s/" || exit 1
cp core/src/*.c "$stage/src/" || exit 1
rm -f "$stage/src/test_reset.c"
cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$stage/src/" || exit 1
cp cpp/include/r2s/runtime.hpp "$stage/src/r2s/" || exit 1
cp cpp/src/runtime.cpp "$stage/src/" || exit 1
# The core never includes the generated header, so the limits travel as -D
# flags read from it: the same set the cmake layout passes as definitions.
extras=$(PYTHONPATH="$root/gen" python3 - "$stage/src/r2s_bundled.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "src/r2s_bundled.h")
except (MessageError, OSError, UnicodeDecodeError) as error:
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
  echo "build-examples-esp32: the extracted flags name no limits ($extras)" >&2
  exit 1
  ;;
esac
# Whether the ELF's symbol table names a symbol: reads the whole table
# first, since grep -q quits on its first match and the closed pipe fails
# the producer under pipefail.
elf_has_symbol() {
  local elf=$1
  local want=$2
  local symbols

  symbols=$(xtensa-esp32-elf-nm "$elf" 2>/dev/null || true)
  case $symbols in
  *"$want"*) return 0 ;;
  *) return 1 ;;
  esac
}
status=0
for sketch in arduino_minimal arduino_hello arduino_blink arduino_button arduino_servo arduino_range \
  arduino_xmacro arduino_static arduino_xmacro_c; do
  out="$build/$sketch/esp32"
  arduino-cli compile --fqbn "esp32:esp32:esp32" \
    --build-property "compiler.c.extra_flags=$extras -fstack-usage" \
    --build-property "compiler.cpp.extra_flags=$extras -fstack-usage" \
    --library "$stage" --build-path "$out/build" --output-dir "$out" "examples/$sketch" || status=1
  # The task sketches register their topics while they run, so each firmware
  # links the runtime topic table; the static C++ and C sketches link the
  # receive buffer instead and must not link the runtime table.
  case "$sketch" in
  arduino_minimal)
    if ! elf_has_symbol "$out/$sketch.ino.elf" r2s_runtime_subscribers; then
      echo "arduino_minimal (esp32): the firmware links no subscriber records" >&2
      status=1
    fi
    ;;
  arduino_hello | arduino_blink | arduino_button | arduino_servo | arduino_range)
    if ! elf_has_symbol "$out/$sketch.ino.elf" r2s_runtime_topics; then
      echo "$sketch (esp32): the firmware links no runtime topic table" >&2
      status=1
    fi
    ;;
  arduino_xmacro)
    if ! elf_has_symbol "$out/$sketch.ino.elf" r2s_rx_buf; then
      echo "$sketch (esp32): the firmware links no receive buffer" >&2
      status=1
    fi
    ;;
  arduino_static | arduino_xmacro_c)
    if ! elf_has_symbol "$out/$sketch.ino.elf" r2s_rx_buf; then
      echo "$sketch (esp32): the firmware links no receive buffer" >&2
      status=1
    fi
    if elf_has_symbol "$out/$sketch.ino.elf" r2s_runtime_topics; then
      echo "$sketch (esp32): the firmware links the runtime topic table" >&2
      status=1
    fi
    ;;
  esac
  if [ "$sketch" = arduino_xmacro ] || [ "$sketch" = arduino_static ] || [ "$sketch" = arduino_xmacro_c ]; then
    for unused in sensor_msgs_Imu geometry_msgs_Quaternion std_msgs_Header; do
      if elf_has_symbol "$out/$sketch.ino.elf" "r2s_bundled_table_$unused"; then
        echo "$sketch (esp32): the linker kept the unused $unused table" >&2
        status=1
      fi
    done
  fi
done

exit "$status"
