#!/usr/bin/env bash
# cppcheck on the core, its tests and the test support. Vendored code is not
# checked. Part of the LINT section of the suite and of make check-warnings.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
command -v cppcheck >/dev/null 2>&1 || { echo "cppcheck not found: run it in the dev image"; exit 1; }
cppcheck --quiet --error-exitcode=1 --std=c99 --enable=warning,style,portability,performance \
  --inline-suppr --suppress=missingIncludeSystem -D R2S_TESTING -D R2S_WITH_RX_RING \
  -I core/include -I core/tests/support -I core/tests/vendor/unity \
  -i core/tests/vendor core/src core/include core/tests
