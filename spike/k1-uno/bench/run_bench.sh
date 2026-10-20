#!/usr/bin/env bash
# Rebuild the size comparison in docs/research/R1-rosserial-archaeology.md.
# Needs avr-gcc, ARDUINO_CORE (arduino/ArduinoCore-avr at 1.8.6) and
# ROSSERIAL (ros-drivers/rosserial at noetic-devel).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
: "${ARDUINO_CORE:?set ARDUINO_CORE}" "${ROSSERIAL:?set ROSSERIAL}"
out=${OUT:-$(mktemp -d)}
python3 -I "$here/bench/gen_roslib.py" "$out/ros_lib.gen" >/dev/null
mkdir -p "$out/ros_lib"
cp -r "$ROSSERIAL"/rosserial_client/src/ros_lib/* "$out/ros_lib/"
cp "$ROSSERIAL"/rosserial_arduino/src/ros_lib/ros.h "$ROSSERIAL"/rosserial_arduino/src/ros_lib/ArduinoHardware.h "$out/ros_lib/"
cp -r "$out"/ros_lib.gen/* "$out/ros_lib/"
b="$here/tools/abuild.sh"
"$b" "$here/bench/baseline.cpp" "$out/baseline.elf"
"$b" "$here/firmware/k1_spike/k1_spike.cpp" "$out/k1_spike.elf"
EXTRA_SRCS="$out/ros_lib/time.cpp $out/ros_lib/duration.cpp" "$b" "$here/bench/rosserial_pubsub.cpp" "$out/ros_default.elf" -I"$out/ros_lib"
EXTRA_SRCS="$out/ros_lib/time.cpp $out/ros_lib/duration.cpp" "$b" "$here/bench/rosserial_pubsub.cpp" "$out/ros_tuned.elf" -I"$out/ros_lib" -DROSSERIAL_TUNED
for e in baseline k1_spike ros_tuned ros_default; do
  avr-size -A "$out/$e.elf" | awk -v n="$e" '/^\.text/{t=$2}/^\.data/{d=$2}/^\.bss/{b=$2}END{printf "%-12s flash %6d  static RAM %5d\n", n, t+d, d+b}'
done
