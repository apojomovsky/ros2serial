#!/usr/bin/env bash
# Build and run the example timing tests: each example's real loop code on
# the host against a scripted tick, asserting the resync the ticket decides
# (and the catch-up the slice keeps). Builds go under R2S_BUILD_DIR, so the
# tree stays clean. Usage: run.sh [--build-only]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/examples/tests
# One lock for the timing-test build directory, held for the whole run: a
# direct run and a suite run of one tree must not write the same binary.
if [ -z "${R2S_EXAMPLE_TESTS_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_EXAMPLE_TESTS_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/examples/tests/run.sh" "$@"
fi
mkdir -p "$build" || exit 1

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: examples/tests/run.sh [--build-only]" >&2; exit 2; }

cxx=${CXX:-g++}
cc=${CC:-gcc}
c_flags="-std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes"
cxx_flags="-std=c++17 -Werror -Wall -Wextra -Wconversion -Wshadow"

# The sketches build against the bundled library, staged the way the
# arduino_minimal example stage is: the bundle plus the core and runtime.
stage="$build/stage"
rm -rf "$stage"
mkdir -p "$stage/src/r2s" "$build/obj-sketch" || exit 1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli bundle --distro jazzy --out "$stage" || exit 1
cp core/include/r2s/*.h "$stage/src/r2s/" || exit 1
cp cpp/include/r2s/runtime.hpp "$stage/src/r2s/" || exit 1
cp cpp/src/runtime.cpp "$stage/src/" || exit 1
sketch_defs=$(PYTHONPATH="$root/gen" python3 - "$stage/src/r2s_bundled.h" <<'EOF'
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

status=0
# The core compiles once per define set; the sketches share the bundle set.
for source in "$root"/core/src/*.c "$stage/src/r2s_bundled.c"; do
  case "$source" in *test_reset.c) continue ;; esac
  # shellcheck disable=SC2086 # defs holds several -D options.
  $cc $c_flags -I"$stage/src" $sketch_defs -c "$source" \
    -o "$build/obj-sketch/$(basename "$source" .c).o" || status=1
done
# shellcheck disable=SC2086 # defs holds several -D options.
$cxx $cxx_flags -I"$stage/src" -I"$root/ports/arduino/tests/stubs" -I"$root/ports/arduino" $sketch_defs \
  -c "$root/ports/arduino/r2s_arduino_port.cpp" -o "$build/obj-sketch/port.o" || status=1
# shellcheck disable=SC2086 # defs holds several -D options.
$cxx $cxx_flags -I"$stage/src" -I"$root/ports/arduino/tests/stubs" $sketch_defs \
  -c "$root/ports/arduino/tests/stubs/Arduino.cpp" -o "$build/obj-sketch/stub.o" || status=1
# shellcheck disable=SC2086 # defs holds several -D options.
$cxx $cxx_flags -I"$stage/src" -I"$root/core/include" -I"$root/cpp/include" $sketch_defs \
  -c "$stage/src/runtime.cpp" -o "$build/obj-sketch/runtime.o" || status=1
# shellcheck disable=SC2086 # defs holds several -D options.
$cxx $cxx_flags -include Arduino.h -I"$stage/src" -I"$root/ports/arduino/tests/stubs" \
  -I"$root/ports/arduino" -I"$root/core/include" -I"$root/cpp/include" $sketch_defs \
  -c -x c++ "$root/examples/arduino_xmacro/arduino_xmacro.ino" \
  -o "$build/obj-sketch/xmacro.o" || status=1
# shellcheck disable=SC2086 # defs holds several -D options.
$cxx $cxx_flags -include Arduino.h -I"$stage/src" -I"$root/ports/arduino/tests/stubs" \
  -I"$root/ports/arduino" -I"$root/core/include" -I"$root/cpp/include" $sketch_defs \
  -c -x c++ "$root/examples/arduino_minimal/arduino_minimal.ino" \
  -o "$build/obj-sketch/minimal.o" || status=1
# shellcheck disable=SC2086 # defs holds several -D options.
$cxx $cxx_flags -include Arduino.h -I"$stage/src" -I"$root/ports/arduino/tests/stubs" \
  -I"$root/ports/arduino" -I"$root/core/include" -I"$root/cpp/include" $sketch_defs \
  -c -x c++ "$root/tests/e2e/avr_slice/avr_slice.ino" \
  -o "$build/obj-sketch/slice.o" || status=1

# Everything the sketch tests link except the other sketches' objects: the
# core, the bundled descriptors, the port glue and the stubs.
sketch_common=""
for object in "$build"/obj-sketch/*.o; do
  case "$object" in */xmacro.o | */minimal.o | */slice.o | */runtime.o) continue ;; esac
  sketch_common="$sketch_common $object"
done
link_sketch() {
  local name=$1 source=$2 sketch_object=$3 extra=$4
  # shellcheck disable=SC2086 # defs holds several -D options.
  # shellcheck disable=SC2087 # common is a pre-split object list.
  $cxx $cxx_flags -I"$stage/src" -I"$root/ports/arduino/tests/stubs" -I"$root/ports/arduino" \
    -I"$root/core/include" -I"$root/cpp/include" -I"$root/examples/tests" $sketch_defs \
    "$source" "$build/obj-sketch/$sketch_object" $sketch_common $extra -o "$build/$name" || status=1
}
link_sketch test_xmacro_timing "$root/examples/tests/test_xmacro_timing.cpp" xmacro.o ""
link_sketch test_minimal_timing "$root/examples/tests/test_minimal_timing.cpp" minimal.o "$build/obj-sketch/runtime.o"
link_sketch test_slice_timing "$root/examples/tests/test_slice_catchup.cpp" slice.o ""

# The cmake example compiles with its own generated limits, against stub AVR
# headers, with the port functions and main renamed to dead stubs: the host
# test drives one loop step against queue fakes instead.
mkdir -p "$build/obj-cmake" || exit 1
cmake_defs=$(PYTHONPATH="$root/gen" python3 - "$root/examples/cmake_minimal/lib/r2s_generated.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "r2s_generated.h")
except (MessageError, OSError) as error:
    print(f"r2s-gen: error: {error}", file=sys.stderr)
    raise SystemExit(1)
print(" ".join("-D" + define for define in defines))
EOF
) || exit 1
for source in "$root"/core/src/*.c "$root"/examples/cmake_minimal/lib/r2s_generated.c; do
  case "$source" in *test_reset.c) continue ;; esac
  # shellcheck disable=SC2086 # defs holds several -D options.
  $cc $c_flags -I"$root/core/include" -I"$root/examples/cmake_minimal/lib" $cmake_defs -c "$source" \
    -o "$build/obj-cmake/$(basename "$source" .c).o" || status=1
done
# shellcheck disable=SC2086 # defs holds several -D options.
# The renames cannot carry prototypes, so the prototype warning is off for
# this test-only object; the example source itself changes nothing.
$cc $c_flags -Wno-missing-prototypes -I"$root/core/include" -I"$root/examples/cmake_minimal/lib" \
  -I"$root/examples/tests/stubs" $cmake_defs \
  -Dmain=cmake_example_main -Dr2s_port_read=cmake_test_port_read \
  -Dr2s_port_write=cmake_test_port_write -Dr2s_port_tx_free=cmake_test_port_tx_free \
  -Dr2s_publish_battery=cmake_test_count_battery \
  -c "$root/examples/cmake_minimal/main.c" -o "$build/obj-cmake/main.o" || status=1
cmake_common=""
for object in "$build"/obj-cmake/*.o; do
  case "$object" in */main.o) continue ;; esac
  cmake_common="$cmake_common $object"
done
# shellcheck disable=SC2086 # defs holds several -D options.
# shellcheck disable=SC2087 # common is a pre-split object list.
$cc $c_flags -I"$root/core/include" -I"$root/examples/cmake_minimal/lib" \
  -I"$root/examples/tests" $cmake_defs \
  "$root/examples/tests/test_cmake_timing.c" "$build/obj-cmake/main.o" \
  $cmake_common -o "$build/test_cmake_timing" || status=1

[ "$build_only" = 1 ] && exit "$status"
[ "$status" -ne 0 ] && exit "$status"
"$build/test_xmacro_timing" phase || status=1
"$build/test_xmacro_timing" stale || status=1
"$build/test_xmacro_timing" offline || status=1
"$build/test_minimal_timing" phase || status=1
"$build/test_minimal_timing" stale || status=1
"$build/test_minimal_timing" offline || status=1
"$build/test_cmake_timing" phase || status=1
"$build/test_cmake_timing" stale || status=1
"$build/test_cmake_timing" offline || status=1
"$build/test_slice_timing" rate || status=1
"$build/test_slice_timing" catchup || status=1
"$build/test_slice_timing" offline || status=1
exit "$status"
