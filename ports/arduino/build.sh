#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Stage the Arduino library and compile a sketch for a board of
# ports/arduino/boards.list.
# Usage: build.sh [--board NAME] [--sketch DIR] [--lib DIR] [--out DIR]
# [--distro NAME] [--stage-only] [--no-bundle]. Builds go under
# R2S_BUILD_DIR, so the tree stays clean. The sketch's limits travel as
# flags read from the bundled header, since the core never includes it.
# An ESP32 board sources scripts/esp32-core.sh before the compile, so the
# pinned core and its toolchains are on PATH; staging needs neither, so a
# stage-only run stays offline for every board.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/arduino
boards_list="$root/ports/arduino/boards.list"

# One board name per line, skipping the comments and blank lines.
board_names() {
  awk 'NF && $1 !~ /^#/ {print $1}' "$boards_list"
}

# Field 2 (the FQBN) or 3 (the binutils prefix) of one board, or nothing
# when the list names no such board.
board_field() {
  awk -v board="$1" -v column="$2" \
    'NF && $1 !~ /^#/ && $1 == board {print $column}' "$boards_list"
}

board=uno
sketch=examples/arduino_minimal
lib=$build/lib
out=""
out_set=0
distro=jazzy
stage_only=0
no_bundle=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --board|--sketch|--lib|--out|--distro)
      # A missing value must fail, not loop: shift 2 with one argument
      # leaves the option in place and the parse never advances.
      [ "$#" -ge 2 ] || { echo "ports/arduino/build.sh: '$1' needs a value" >&2; exit 2; }
      case "$1" in
        --board) board=$2 ;;
        --sketch) sketch=$2 ;;
        --lib) lib=$2 ;;
        --out) out=$2; out_set=1 ;;
        --distro) distro=$2 ;;
      esac
      shift 2
      ;;
    --stage-only) stage_only=1; shift ;;
    --no-bundle) no_bundle=1; shift ;;
    -h|--help) echo "usage: ports/arduino/build.sh [--board $(board_names | paste -sd'|')] [--sketch DIR] [--lib DIR] [--out DIR] [--distro NAME] [--stage-only] [--no-bundle]" >&2; exit 0 ;;
    *) echo "ports/arduino/build.sh: unknown option '$1'" >&2; exit 2 ;;
  esac
done
fqbn=$(board_field "$board" 2)
[ -n "$fqbn" ] || {
  echo "ports/arduino/build.sh: unknown board '$board': known are $(board_names | paste -sd' ')" >&2
  exit 2
}
[ "$out_set" = 1 ] || out="build/$board"
[ -d "$sketch" ] || { echo "ports/arduino/build.sh: no sketch at '$sketch'" >&2; exit 2; }

if [ "$stage_only" = 0 ]; then
  command -v arduino-cli >/dev/null 2>&1 || { echo "ports/arduino/build.sh: arduino-cli not found" >&2; exit 1; }
  case "$fqbn" in
    esp32:*)
      # shellcheck source=scripts/esp32-core.sh
      source "$root/scripts/esp32-core.sh" || exit 1
      ;;
    *)
      # The dev image carries the pinned core, so a present core stays
      # untouched and no network is needed.
      if arduino-cli core list | grep -q "arduino:avr"; then
        echo "arduino build: arduino:avr core already installed"
      else
        arduino-cli core install arduino:avr || exit 1
      fi
      ;;
  esac
fi

if [ "$no_bundle" = 0 ]; then
  rm -rf "$lib"
  mkdir -p "$lib/src/r2s" || exit 1
  PYTHONPATH="$root/gen" python3 -m r2s_gen.cli bundle --distro "$distro" --out "$lib" || exit 1
  cp core/include/r2s/*.h "$lib/src/r2s/" || exit 1
  cp core/src/*.c "$lib/src/" || exit 1
  rm -f "$lib/src/test_reset.c"
  cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$lib/src/" || exit 1
  cp cpp/include/r2s/runtime.hpp "$lib/src/r2s/" || exit 1
  cp cpp/src/runtime.cpp "$lib/src/" || exit 1
  cp cpp/include/r2s/action.hpp "$lib/src/r2s/" || exit 1
  cp cpp/src/action.cpp "$lib/src/" || exit 1
  echo "arduino build: library staged at $lib"
fi

# The core never includes the bundled header, so the limits travel as -D
# flags read from it. A flag string without the limits would compile a
# firmware that silently ignores every subscription, so every value must
# be present and non-empty.
extras=$(PYTHONPATH="$root/gen" python3 - "$lib/src/r2s_bundled.h" <<'EOF'
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
case "$extras" in
  *R2S_WITH_CODEC*R2S_MAX_FRAME=[0-9]*R2S_MAX_NEST=[0-9]*) ;;
  *) echo "arduino build: the extracted flags name no limits ($extras)" >&2; exit 1 ;;
esac
echo "arduino build: limits $extras"
[ "$stage_only" = 1 ] && exit 0

arduino-cli compile --fqbn "$fqbn" \
  --build-property "compiler.c.extra_flags=$extras" \
  --build-property "compiler.cpp.extra_flags=$extras" \
  --library "$lib" --output-dir "$out" "$sketch" || exit 1
echo "arduino build: firmware at $out"
