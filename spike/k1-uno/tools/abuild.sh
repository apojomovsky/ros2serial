#!/usr/bin/env bash
# Build an Arduino Uno sketch without arduino-cli, using the Arduino AVR
# core 1.8.6 flags (platform.txt), so sizes match an IDE build.
# Usage: abuild.sh <sketch.cpp> <out.elf> [extra -I/-D flags...]
# EXTRA_SRCS: space-separated library sources compiled with the same flags.
# Needs ARDUINO_CORE pointing at a checkout of arduino/ArduinoCore-avr 1.8.6.
set -euo pipefail
src=$1; out=$2; shift 2
core=${ARDUINO_CORE:?set ARDUINO_CORE to an ArduinoCore-avr 1.8.6 checkout}
mcu=${MCU:-atmega328p}; variant=${VARIANT:-standard}; board=${BOARD:-ARDUINO_AVR_UNO}
common="-mmcu=$mcu -DF_CPU=16000000L -DARDUINO=10819 -D$board -DARDUINO_ARCH_AVR -I$core/cores/arduino -I$core/variants/$variant"
cflags="-c -g -Os -w -std=gnu11 -ffunction-sections -fdata-sections -flto -fno-fat-lto-objects $common"
cxxflags="-c -g -Os -w -std=gnu++11 -fpermissive -fno-exceptions -ffunction-sections -fdata-sections -fno-threadsafe-statics -Wno-error=narrowing -flto $common"
cache=${ABUILD_CACHE:-$(dirname "$out")/core-$mcu}
if [ ! -f "$cache/core.a" ]; then
  mkdir -p "$cache"
  for f in "$core"/cores/arduino/*.c; do avr-gcc $cflags "$f" -o "$cache/$(basename "$f").o"; done
  for f in "$core"/cores/arduino/*.cpp; do avr-g++ $cxxflags "$f" -o "$cache/$(basename "$f").o"; done
  for f in "$core"/cores/arduino/*.S; do avr-gcc -c -g -x assembler-with-cpp -flto $common "$f" -o "$cache/$(basename "$f").o"; done
  avr-gcc-ar rcs "$cache/core.a" "$cache"/*.o
fi
obj=${out%.elf}.o
case "$src" in
  *.c) avr-gcc $cflags "$@" "$src" -o "$obj" ;;
  *)   avr-g++ $cxxflags "$@" "$src" -o "$obj" ;;
esac
objs=("$obj")
for f in ${EXTRA_SRCS:-}; do
  o=${out%.elf}-$(basename "$f").o
  case "$f" in
    *.c) avr-gcc $cflags "$@" "$f" -o "$o" ;;
    *)   avr-g++ $cxxflags "$@" "$f" -o "$o" ;;
  esac
  objs+=("$o")
done
avr-gcc -w -Os -g -flto -fuse-linker-plugin -Wl,--gc-sections -mmcu=$mcu -o "$out" "${objs[@]}" "$cache/core.a" -lm
