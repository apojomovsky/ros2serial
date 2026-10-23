#!/usr/bin/env bash
# Build the simavr harness and the echo firmware for the ATmega328P and the
# ATmega2560, then run the harness's tests. Builds go under R2S_BUILD_DIR, so
# the tree stays clean; the suite runs this script for E2E=avr.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/harness/simavr
mkdir -p "$build" || exit 1

status=0
cc -std=c99 -Wall -Wextra -Werror -O2 -o "$build/r2s_simavr" tests/harness/simavr/r2s_simavr.c \
  -lsimavr -lelf || status=1
for mcu in atmega328p atmega2560; do
  for image in echo deep; do
    avr-gcc -mmcu="$mcu" -DF_CPU=16000000UL -Os -Wall -Wextra -Werror \
      -o "$build/$image-$mcu.elf" "tests/harness/simavr/${image}_firmware.c" || status=1
  done
done
if [ "$status" -ne 0 ]; then
  echo "the simavr harness or its firmware did not build" >&2
  exit 1
fi

R2S_SIMAVR_BIN="$build/r2s_simavr" R2S_SIMAVR_FW="$build" python3 -m pytest tests/harness/simavr -q
