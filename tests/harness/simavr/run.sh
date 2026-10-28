#!/usr/bin/env bash
# Build the simavr harness and its firmware images for the ATmega328P and the
# ATmega2560, then run the harness's tests. Builds go under R2S_BUILD_DIR, so
# the tree stays clean; the suite runs this script for E2E=avr.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/harness/simavr
# One lock for the harness build directory, held for the whole run: a direct
# run and a suite run of one tree must not write the same ELF and binary.
if [ -z "${R2S_SIMAVR_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_SIMAVR_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/tests/harness/simavr/run.sh" "$@"
fi

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: tests/harness/simavr/run.sh [--build-only]" >&2; exit 2; }

status=0
cc -std=c99 -Wall -Wextra -Werror -O2 -o "$build/r2s_simavr" tests/harness/simavr/r2s_simavr.c \
  -lsimavr -lelf || status=1
for mcu in atmega328p atmega2560; do
  for image in echo deep; do
    avr-gcc -mmcu="$mcu" -DF_CPU=16000000UL -Os -Wall -Wextra -Werror \
      -o "$build/$image-$mcu.elf" "tests/harness/simavr/${image}_firmware.c" || status=1
  done
  # The announce firmware links the core, so the topic table announces
  # through the same sources the host tests cover. Built twice from one
  # source: the flash image binds its PROGMEM table, the RAM image the same
  # table from RAM through the runtime entry points.
  mapfile -t core_sources < <(ls "$root"/core/src/*.c | grep -v test_reset.c)
  avr-gcc -mmcu="$mcu" -DF_CPU=16000000UL -Os -Wall -Wextra -Werror -I "$root/core/include" \
    -o "$build/announce-$mcu.elf" tests/harness/simavr/announce_firmware.c "${core_sources[@]}" || status=1
  avr-gcc -mmcu="$mcu" -DF_CPU=16000000UL -Os -Wall -Wextra -Werror -DR2S_ANNOUNCE_RAM_TABLE \
    -I "$root/core/include" -o "$build/announce-ram-$mcu.elf" tests/harness/simavr/announce_firmware.c \
    "${core_sources[@]}" || status=1
done
if [ "$status" -ne 0 ]; then
  echo "the simavr harness or its firmware did not build" >&2
  exit 1
fi
[ "$build_only" = 1 ] && exit 0

R2S_SIMAVR_BIN="$build/r2s_simavr" R2S_SIMAVR_FW="$build" python3 -m pytest tests/harness/simavr -q
