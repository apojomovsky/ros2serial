#!/usr/bin/env bash
# The warnings gate: the core builds clean with -Werror under gcc and clang
# (the flags are in core/CMakeLists.txt), and cppcheck reports nothing on the
# core and its test support. Vendored code is not checked.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
status=0
bash scripts/core-test.sh --build-only gcc clang || status=1
if command -v cppcheck >/dev/null 2>&1; then
  cppcheck --quiet --error-exitcode=1 --std=c99 --enable=warning,style,portability,performance \
    --inline-suppr --suppress=missingIncludeSystem -I core/include -I core/tests/support \
    -i core/tests/vendor core/src core/include core/tests/support || status=1
else
  echo "cppcheck not found: run it in the dev image"
  status=1
fi
exit $status
