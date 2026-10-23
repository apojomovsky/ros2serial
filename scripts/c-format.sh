#!/usr/bin/env bash
# Check (default) or apply (--fix) clang-format on the tracked C and C++ files,
# or on the files given. Vendored code under */vendor/ is never touched.
# Usage: c-format.sh [--fix] [FILE...]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
mode=(--dry-run --Werror)
if [ "${1:-}" = --fix ]; then mode=(-i); shift; fi
command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found: run it in the dev image"; exit 1; }
if [ "$#" -gt 0 ]; then
  files=("$@")
else
  mapfile -t files < <(git ls-files -- '*.c' '*.h' '*.cpp' '*.hpp' '*.cc' ':!:*/vendor/*')
fi
mapfile -t files < <(printf '%s\n' "${files[@]}" | grep -Ev '(^|/)vendor/' | grep -E '\.(c|h|cpp|hpp|cc)$')
[ "${#files[@]}" -gt 0 ] || exit 0
clang-format "${mode[@]}" -- "${files[@]}"
