#!/usr/bin/env bash
# The warnings gate: the core builds clean with -Werror under gcc and clang
# (the flags are in core/CMakeLists.txt), the fuzz targets compile under the
# same flags, and cppcheck reports nothing.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
status=0
bash "$root/scripts/core-test.sh" --build-only gcc clang || status=1

# The fuzz targets are in no CMake target yet (C11 wires them into the
# fuzz-smoke job), so they are compiled here with the core's own flags.
for source in "$root"/core/fuzz/*.c; do
  [ -e "$source" ] || continue
  clang -std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow \
    -Wstrict-prototypes -Wmissing-prototypes -Wundef \
    -I "$root/core/include" -fsyntax-only "$source" || status=1
done

bash "$root/scripts/cppcheck.sh" || status=1
exit $status
