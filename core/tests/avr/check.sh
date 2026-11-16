#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# AVR flash check for C12: the R2S_FLASH tables and the flash-string log calls
# keep strings out of RAM. Builds the check firmware with and without the log
# lines and fails when the SRAM use differs; avr-strings proves the lines
# survived, so an optimizing build cannot pass by dropping them. A third build
# without R2S_WITH_LOG proves the logging calls compile out for tiers without
# logging. A fourth firmware binds an R2S_FLASH service table under simavr, so
# the placement the generator emits stays covered on the target where a plain
# read fetches the wrong address space. Builds go under R2S_BUILD_DIR, so the
# tree stays clean.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/core/avr-flash
# One lock for the check directory, held for the whole run.
if [ -z "${R2S_AVR_FLASH_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_AVR_FLASH_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/core/tests/avr/check.sh" "$@"
fi
mkdir -p "$build" || exit 1

mapfile -t sources < <(ls "$root"/core/src/*.c | grep -v test_reset.c)
# The frame limit is a build input no build may leave out (#640). This check
# compares two builds of itself, so it pins the value the measured SRAM
# baselines were taken at.
common=(-mmcu=atmega328p -std=c99 -Os -Wall -Wextra -Werror -Wconversion -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes -Wundef -I "$root/core/include"
  -DR2S_MAX_FRAME=128)
firmware="$root/core/tests/avr/flash_log.c"

sram() {
  avr-size -A "$build/$1.elf" | awk '/^\.data/ { found = 1; data = $2 } /^\.bss/ { bss = $2 } END { if (!found) { exit 1 }; print data + bss }'
}

# With logging, without the log lines: the SRAM baseline.
avr-gcc "${common[@]}" -DR2S_WITH_LOG -o "$build/base.elf" "${sources[@]}" "$firmware" \
  || { echo "avr-flash: base firmware did not build"; exit 1; }
# With logging and the log lines: the strings must not add RAM.
avr-gcc "${common[@]}" -DR2S_WITH_LOG -DR2S_AVR_CHECK_LOGS -o "$build/logs.elf" "${sources[@]}" "$firmware" \
  || { echo "avr-flash: log firmware did not build"; exit 1; }
# Without logging: the calls compile out for tiers without logging. The log
# lines stay on, so the no-op macros must accept them without warnings.
avr-gcc "${common[@]}" -DR2S_AVR_CHECK_LOGS -o "$build/nolog.elf" "${sources[@]}" "$firmware" \
  || { echo "avr-flash: firmware without R2S_WITH_LOG did not build"; exit 1; }

base_ram=$(sram base)
logs_ram=$(sram logs)
[ -n "$base_ram" ] && [ -n "$logs_ram" ] || { echo "avr-flash: avr-size gave no SRAM use"; exit 1; }
[ "$base_ram" = "$logs_ram" ] \
  || { echo "avr-flash: log lines added $((logs_ram - base_ram)) bytes of RAM"; exit 1; }
for line in "battery low log line" "adc counts=" "link lost log line"; do
  # No -q: grep quitting on the first match closes the pipe while avr-strings
  # still writes, and pipefail turns its SIGPIPE into a false missing line.
  avr-strings "$build/logs.elf" | grep -F "$line" > /dev/null \
    || { echo "avr-flash: log line missing from flash: $line"; exit 1; }
done

# The service table the generator emits in flash: the bind, the role and the
# string walk, then the DECLARE a QUERY answers, all on the target where a
# plain read would fetch the wrong address space. simavr ends the run when the
# firmware sleeps after printing its verdict.
avr-gcc "${common[@]}" -DF_CPU=16000000UL -DR2S_WITH_SERVICES -o "$build/services.elf" "${sources[@]}" \
  "$root/core/tests/avr/flash_services.c" \
  || { echo "avr-flash: service firmware did not build"; exit 1; }
services_log="$build/services.log"
timeout 30 simavr -m atmega328p -f 16000000 "$build/services.elf" >"$services_log" 2>&1 \
  || { echo "avr-flash: the service firmware did not run"; exit 1; }
grep -Fq "r2s-flash-services: PASS" "$services_log" \
  || { echo "avr-flash: the flash service table did not bind:"; cat "$services_log"; exit 1; }
echo "avr-flash ok: SRAM unchanged at ${base_ram} bytes, log lines in flash, flash services bound"
