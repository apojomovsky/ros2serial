#!/usr/bin/env bash
# Build one example per generate layout: the cmake firmware for the ATmega328P
# and the Arduino sketch for Uno and Mega. The layout outputs are checked in
# under examples/ and pinned by the generator tests; this script only
# assembles and compiles, so a contributor and CI share it. Builds go under
# R2S_BUILD_DIR, so the tree stays clean.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/examples
# One lock for the examples build directory, held for the whole run: a direct
# run and a suite run of one tree must not write the same ELF at once.
if [ -z "${R2S_EXAMPLES_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_EXAMPLES_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/scripts/build-examples.sh" "$@"
fi
mkdir -p "$build" || exit 1

# The RAM the runtime path costs, printed from the linked ELF: the topic
# table the core announces, the records beside it, and the marginal cost of
# one topic. The table is sized for the worst case, so the per-topic figure
# divides the table by its capacity, not by the topics the sketch registered.
# The subscriber's message storage is no runtime symbol: it lives in the
# sketch's own Subscription object, so its size is printed too.
report_runtime_ram() {
  local elf=$1 board=$2 sketch=$3 flags=$4
  local table publishers subscribers capacity per_topic
  local battery_pub cmd_vel_sub registered

  table=$(avr-nm --print-size "$elf" 2>/dev/null | awk '$4 == "r2s_runtime_topics" { print $2; exit }')
  publishers=$(avr-nm --print-size "$elf" 2>/dev/null | awk '$4 == "r2s_runtime_publishers" { print $2; exit }')
  subscribers=$(avr-nm --print-size "$elf" 2>/dev/null | awk '$4 == "r2s_runtime_subscribers" { print $2; exit }')
  if [ -z "$table" ]; then
    echo "arduino_minimal ($board): the firmware links no runtime topic table" >&2
    return 1
  fi
  # avr-nm prints the sizes in hex, so they are read as hex numbers.
  table=$((16#${table}))
  publishers=$((16#${publishers:-0}))
  subscribers=$((16#${subscribers:-0}))
  # The table is sized for the build's topic limit, which the flags raise
  # and the core's header defaults.
  capacity=$(printf '%s\n' "$flags" | awk '{ for (i = 1; i < NF; i++) if ($i ~ /R2S_TOPIC_MAX_COUNT=/) print $i }' |
    cut -d= -f2)
  if [ -z "$capacity" ]; then
    capacity=$(awk '/^#define R2S_TOPIC_MAX_COUNT/ { print $3 }' "$root/core/include/r2s/r2s_topics.h" | tr -d 'u')
  fi
  per_topic=0
  if [ -n "$capacity" ] && [ "$capacity" -gt 0 ]; then
    per_topic=$((table / capacity))
  fi
  battery_pub=$(avr-nm --print-size "$elf" 2>/dev/null | awk '$4 == "battery_pub" { print $2; exit }')
  cmd_vel_sub=$(avr-nm --print-size "$elf" 2>/dev/null | awk '$4 == "cmd_vel_sub" { print $2; exit }')
  battery_pub=$((16#${battery_pub:-0}))
  cmd_vel_sub=$((16#${cmd_vel_sub:-0}))
  registered=$(grep -c 'r2s::\(Publisher\|Subscription\)<' "$sketch")
  echo "arduino_minimal ($board): runtime RAM"
  echo "  topic table: $table bytes for $capacity topics ($per_topic bytes per topic)"
  echo "  publisher records: $publishers bytes"
  echo "  subscriber records: $subscribers bytes"
  echo "  sketch objects: battery_pub $battery_pub bytes, cmd_vel_sub $cmd_vel_sub bytes"
  echo "  topics the sketch registers: $registered"
}

status=0
for sketch in examples/arduino_minimal/arduino_minimal.ino examples/arduino_xmacro/arduino_xmacro.ino; do
  # The port glue lives in the library now: a sketch defining a port function
  # or opening its own linkage block would shadow or split it, so it fails.
  # Calls and comments still name the port API, so only a definition counts:
  # the five port functions with their contract return types.
  if grep -n -e '^\(uint16_t \|void \|uint32_t \)r2s_port_' -e 'extern "C" {' "$sketch"; then
    echo "$sketch: the sketch must not define port functions or linkage blocks" >&2
    status=1
  fi
done
cmake -S examples/cmake_minimal -B "$build/cmake_minimal" \
  --toolchain "$root/examples/cmake_minimal/avr-toolchain.cmake" \
  -DCMAKE_C_FLAGS="-fstack-usage" || status=1
cmake --build "$build/cmake_minimal" || status=1
if [ "$status" -eq 0 ]; then
  avr-size "$build/cmake_minimal/hello_cmake" || status=1
fi

# The runtime sketch builds against the bundled library: r2s-gen emits the
# library at build time, the core and the C++ runtime layer join it here,
# and no r2s.yaml is involved. The sketch registers its topics while it
# runs, so the library carries the runtime API and the message headers
# instead of a generated topic table.
stage="$build/arduino_minimal"
rm -rf "$stage"
mkdir -p "$stage/src/r2s" || status=1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli bundle --distro jazzy --out "$stage" || exit 1
cp core/include/r2s/*.h "$stage/src/r2s/" || status=1
cp core/src/*.c "$stage/src/" || status=1
rm -f "$stage/src/test_reset.c"
cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$stage/src/" || status=1
cp cpp/include/r2s/runtime.hpp "$stage/src/r2s/" || status=1
cp cpp/src/runtime.cpp "$stage/src/" || status=1
# The core never includes the generated header, so the limits travel as -D
# flags read from it: the same set the cmake layout passes as definitions.
# A library cannot set its own flags on this toolchain, hence --build-property.
extras=$(PYTHONPATH="$root/gen" python3 - "$stage/src/r2s_bundled.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "src/r2s_bundled.h")
except (MessageError, OSError) as error:
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
        echo "build-examples: the extracted flags name no limits ($extras)" >&2
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

    symbols=$(avr-nm "$elf" 2>/dev/null || true)
    case $symbols in
        *"$want"*) return 0 ;;
        *) return 1 ;;
    esac
}
for board in uno mega; do
  out="$build/arduino_minimal/$board"
  arduino-cli compile --fqbn "arduino:avr:$board" \
    --build-property "compiler.c.extra_flags=$extras -fstack-usage" \
    --build-property "compiler.cpp.extra_flags=$extras -fstack-usage" \
    --library "$stage" --build-path "$out/build" --output-dir "$out" examples/arduino_minimal || status=1
  # The subscriber records prove the codec flag reached the core. The
  # runtime path has no generated receive buffer like the static one, but
  # the records are only reachable through the dispatch hook, and the
  # linker drops them when the core never dispatches a user frame.
  if ! elf_has_symbol "$out/arduino_minimal.ino.elf" r2s_runtime_subscribers; then
    echo "arduino_minimal ($board): the firmware links no subscriber records" >&2
    status=1
  fi
  report_runtime_ram "$out/arduino_minimal.ino.elf" "$board" examples/arduino_minimal/arduino_minimal.ino "$extras"
done

# The X-macro sketch builds against the bundled library: r2s-gen emits the
# library at build time, the core joins it like above, and no r2s.yaml is
# involved. The sketch uses two publishers and two subscribers, so the
# Imu and Quaternion tables must be absent from the firmware: the linker
# drops the bundled tables no topic names.
xstage="$build/arduino_xmacro"
rm -rf "$xstage"
mkdir -p "$xstage/src/r2s" || status=1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli bundle --distro jazzy --out "$xstage" || exit 1
cp core/include/r2s/*.h "$xstage/src/r2s/" || status=1
cp core/src/*.c "$xstage/src/" || status=1
rm -f "$xstage/src/test_reset.c"
cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$xstage/src/" || status=1
xextras=$(PYTHONPATH="$root/gen" python3 - "$xstage/src/r2s_bundled.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "src/r2s_bundled.h")
except (MessageError, OSError) as error:
    print(f"r2s-gen: error: {error}", file=sys.stderr)
    raise SystemExit(1)
print(" ".join("-D" + define for define in defines))
EOF
) || exit 1
case "$xextras" in
    *R2S_WITH_CODEC*R2S_MAX_FRAME=[0-9]*R2S_MAX_NEST=[0-9]*) ;;
    *)
        echo "build-examples: the extracted bundle flags name no limits ($xextras)" >&2
        exit 1
        ;;
esac
for board in uno mega; do
  out="$build/arduino_xmacro/$board"
  arduino-cli compile --fqbn "arduino:avr:$board" --build-property "compiler.c.extra_flags=$xextras -fstack-usage" \
    --build-property "compiler.cpp.extra_flags=$xextras -fstack-usage" \
    --library "$xstage" --build-path "$out/build" --output-dir "$out" examples/arduino_xmacro || status=1
  if ! elf_has_symbol "$out/arduino_xmacro.ino.elf" r2s_rx_buf; then
    echo "arduino_xmacro ($board): the firmware links no receive buffer" >&2
    status=1
  fi
  for unused in sensor_msgs_Imu geometry_msgs_Quaternion std_msgs_Header; do
    if elf_has_symbol "$out/arduino_xmacro.ino.elf" "r2s_bundled_table_$unused"; then
      echo "arduino_xmacro ($board): the linker kept the unused $unused table" >&2
      status=1
    fi
  done
done

# The stack budgets need .su files, which avr-gcc does not write under the
# shipped link-time optimized flags, so the budget gate rebuilds each sketch
# without LTO under stack/. Same sources and defines, only the optimizer
# differs, and the deeper non-LTO paths overestimate rather than hide.
if [ -n "${R2S_STACK_BUILD:-}" ]; then
  for sketch in arduino_minimal arduino_xmacro; do
    if [ "$sketch" = arduino_minimal ]; then
      sstage=$stage
      sflags=$extras
    else
      sstage=$xstage
      sflags=$xextras
    fi
    for board in uno mega; do
      sout="$build/stack/$sketch/$board"
      arduino-cli compile --fqbn "arduino:avr:$board" \
        --build-property "compiler.c.extra_flags=$sflags -fno-lto -fstack-usage" \
        --build-property "compiler.cpp.extra_flags=$sflags -fno-lto -fstack-usage" \
        --build-property "compiler.c.elf.extra_flags=-fno-lto" \
        --library "$sstage" --build-path "$sout" "examples/$sketch" || status=1
    done
  done
fi

# The example timing tests drive each loop on the host against a scripted
# tick: resync for the examples, catch-up for the slice. No DDS is involved.
bash "$root/examples/tests/run.sh" || status=1

exit "$status"
