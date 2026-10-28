#!/usr/bin/env bash
# Syntax-only compile of the C++ layer's headers with avr-g++, so the code
# that reaches a board is checked against the toolchain that builds it.
# The message headers include the bundle's C header, so one is staged where
# they find it; nothing is written to the tree.
# Usage: avr-syntax.sh
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/cpp/avr-syntax
mkdir -p "$build" || exit 1

if ! command -v avr-g++ >/dev/null 2>&1; then
  echo "avr-g++ not found: run this in the dev image"
  exit 1
fi

flags=(-mmcu=atmega328p -std=gnu++11 -Os -Wall -Wextra -Werror -Wconversion -Wshadow
  -fno-exceptions -fno-rtti -I "$root/core/include" -I "$root/cpp/include")

# The bundle's C header, where the message headers look for it. The tree
# carries the frozen copy the generator emits; the golden file stands in
# when it is not there yet, so this check does not depend on the generator
# running.
bundled="$root/cpp/include/r2s_bundled.h"
if [ ! -f "$bundled" ]; then
  bundled="$root/gen/tests/golden/bundle/r2s_bundled.h"
fi
if [ ! -f "$bundled" ]; then
  echo "avr-syntax: no r2s_bundled.h to compile the message headers against"
  exit 1
fi
stage="$build/stage"
rm -rf "$stage"
mkdir -p "$stage"
cp "$bundled" "$stage/" || exit 1
flags+=(-I "$stage")

# One translation unit per header, so a header that breaks the toolchain
# fails here and names itself. The header is included by the path a sketch
# uses, with the package directory on the include path.
sources=()
for header in "$root"/cpp/include/*/msg/*.hpp; do
  [ -e "$header" ] || continue
  name=$(basename "$header" .hpp)
  package=$(basename "$(dirname "$(dirname "$header")")")
  printf '#include <%s/msg/%s.hpp>\n' "$package" "$name" >"$build/check_$name.cpp"
  sources+=("$build/check_$name.cpp")
done
# The runtime header on its own, without a message type, so a change there
# fails even when no message header exists yet.
printf '#include "r2s/runtime.hpp"\n' >"$build/check_runtime.cpp"
sources+=("$build/check_runtime.cpp")
# The headers as a sketch includes them, with the package directories on
# the path, so the spelling a sketch uses is the one that compiles.
printf '#include <std_msgs/msg/float32.hpp>\n#include <geometry_msgs/msg/twist.hpp>\n' \
  >"$build/check_sketch.cpp"
sources+=("$build/check_sketch.cpp")

status=0
for source in "${sources[@]}"; do
  if ! avr-g++ "${flags[@]}" -fsyntax-only "$source"; then
    echo "avr-syntax: $source did not compile"
    status=1
  fi
done
# The runtime's own source, since a board compiles it as well. The AVR
# toolchains carry no C++ standard library, so this catches a header the
# host build accepts and the board does not.
if ! avr-g++ "${flags[@]}" -fsyntax-only "$root/cpp/src/runtime.cpp"; then
  echo "avr-syntax: cpp/src/runtime.cpp did not compile"
  status=1
fi
[ "$status" = 0 ] || exit 1
echo "avr-syntax ok: ${#sources[@]} translation units and the runtime source"
