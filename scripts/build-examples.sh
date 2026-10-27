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

status=0
cmake -S examples/cmake_minimal -B "$build/cmake_minimal" \
  --toolchain "$root/examples/cmake_minimal/avr-toolchain.cmake" || status=1
cmake --build "$build/cmake_minimal" || status=1
if [ "$status" -eq 0 ]; then
  avr-size "$build/cmake_minimal/hello_cmake" || status=1
fi

# The Arduino library is staged at build time: the checked-in lib holds only
# what the generator emits, and the core sources join it here, so no core
# bytes live twice in the repository and nothing can drift.
stage="$build/arduino_minimal"
rm -rf "$stage"
mkdir -p "$stage/src/r2s" || status=1
cp examples/arduino_minimal/lib/library.properties "$stage/" || status=1
cp examples/arduino_minimal/lib/src/r2s_generated.h examples/arduino_minimal/lib/src/r2s_generated.c \
  "$stage/src/" || status=1
cp core/include/r2s/*.h "$stage/src/r2s/" || status=1
cp core/src/*.c "$stage/src/" || status=1
rm -f "$stage/src/test_reset.c"
# The core never includes the generated header, so the limits travel as -D
# flags read from it: the same set the cmake layout passes as definitions.
# A library cannot set its own flags on this toolchain, hence --build-property.
extras=$(PYTHONPATH="$root/gen" python3 - "$stage/src/r2s_generated.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "src/r2s_generated.h")
except MessageError as error:
    print(f"r2s-gen: error: {error}", file=sys.stderr)
    raise SystemExit(1)
print(" ".join("-D" + define for define in defines))
EOF
) || exit 1
# An empty flag string would compile a firmware that silently ignores every
# subscription, so the limits must travel in it, not just the codec flag.
case "$extras" in
    *R2S_WITH_CODEC*R2S_MAX_FRAME=*R2S_MAX_NEST=*) ;;
    *)
        echo "build-examples: the extracted flags name no limits ($extras)" >&2
        exit 1
        ;;
esac
for board in uno mega; do
  out="$build/arduino_minimal/$board"
  arduino-cli compile --fqbn "arduino:avr:$board" --build-property "compiler.c.extra_flags=$extras" \
    --library "$stage" --output-dir "$out" examples/arduino_minimal || status=1
  # The receive buffer proves the codec flag reached the core: it exists only
  # with the codec enabled, and without it the linker drops the buffer while
  # the firmware silently ignores every subscription. The limits travel in
  # the same flag string, pinned by the generator's own tests.
  if ! avr-nm "$out/arduino_minimal.ino.elf" 2>/dev/null | grep -q r2s_rx_buf; then
    echo "arduino_minimal ($board): the firmware links no receive buffer" >&2
    status=1
  fi
done

exit "$status"
