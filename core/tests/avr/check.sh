#!/usr/bin/env bash
# AVR flash check for C12: the R2S_FLASH tables and the flash-string log calls
# keep strings out of RAM. Builds the check firmware with and without the log
# lines and fails when the SRAM use differs; avr-strings proves the lines
# survived, so an optimizing build cannot pass by dropping them. A third build
# without R2S_WITH_LOG proves the logging calls compile out for tiers without
# logging. Builds go under R2S_BUILD_DIR, so the tree stays clean.
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
common=(-mmcu=atmega328p -std=c99 -Os -Wall -Wextra -Werror -Wconversion -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes -Wundef -I "$root/core/include")
firmware="$root/core/tests/avr/flash_log.c"

sram() {
  avr-size -A "$build/$1.elf" | awk '/^\.data/ { data = $2 } /^\.bss/ { bss = $2 } END { print data + bss }'
}

# With logging, without the log lines: the SRAM baseline.
avr-gcc "${common[@]}" -DR2S_WITH_LOG -o "$build/base.elf" "${sources[@]}" "$firmware" \
  || { echo "avr-flash: base firmware did not build"; exit 1; }
# With logging and the log lines: the strings must not add RAM.
avr-gcc "${common[@]}" -DR2S_WITH_LOG -DR2S_AVR_CHECK_LOGS -o "$build/logs.elf" "${sources[@]}" "$firmware" \
  || { echo "avr-flash: log firmware did not build"; exit 1; }
# Without logging: the calls compile out for tiers without logging.
avr-gcc "${common[@]}" -o "$build/nolog.elf" "${sources[@]}" "$firmware" \
  || { echo "avr-flash: firmware without R2S_WITH_LOG did not build"; exit 1; }

base_ram=$(sram base)
logs_ram=$(sram logs)
[ -n "$base_ram" ] && [ -n "$logs_ram" ] || { echo "avr-flash: avr-size gave no SRAM use"; exit 1; }
[ "$base_ram" = "$logs_ram" ] \
  || { echo "avr-flash: log lines added $((logs_ram - base_ram)) bytes of RAM"; exit 1; }
for line in "battery low log line" "adc counts=" "link lost log line"; do
  avr-strings "$build/logs.elf" | grep -qF "$line" \
    || { echo "avr-flash: log line missing from flash: $line"; exit 1; }
done
echo "avr-flash ok: SRAM unchanged at ${base_ram} bytes, log lines in flash"
