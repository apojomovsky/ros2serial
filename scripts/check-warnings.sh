#!/usr/bin/env bash
# The warnings gate: the core builds clean with -Werror under gcc and clang
# (the flags are in core/CMakeLists.txt), and cppcheck reports nothing.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
status=0
bash "$root/scripts/core-test.sh" --build-only gcc clang || status=1
bash "$root/scripts/cppcheck.sh" || status=1
exit $status
