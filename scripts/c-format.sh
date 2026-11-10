#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Check (default) or apply (--fix) the ament uncrustify style on the tracked C
# and C++ files, or on the files given. Vendored code under */vendor/ is never
# touched, and neither is the bridge: ament_lint_auto checks it instead (#509).
# Usage: c-format.sh [--fix] [FILE...]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
fix=0
if [ "${1:-}" = --fix ]; then fix=1; shift; fi
command -v uncrustify >/dev/null 2>&1 || { echo "uncrustify not found: run it in the dev image"; exit 1; }
version=$(uncrustify --version 2>/dev/null | grep -o '[0-9][0-9.]*' | head -n 1)
[ "$version" = "0.78.1" ] || {
  echo "uncrustify $version is not the pinned 0.78.1: refresh tools/vendor/ament_code_style.cfg"
  exit 1
}
cfg="$root/tools/vendor/ament_code_style.cfg"
[ -f "$cfg" ] || { echo "missing config: $cfg"; exit 1; }
if [ "$#" -gt 0 ]; then
  files=("$@")
else
  mapfile -t files < <(git ls-files -- '*.c' '*.h' '*.cpp' '*.hpp' '*.cc' ':!:*/vendor/*' ':!:bridge/**')
fi
mapfile -t files < <(printf '%s\n' "${files[@]}" | grep -Ev '(^|/)vendor/|^bridge/' | grep -E '\.(c|h|cpp|hpp|cc)$')
[ "${#files[@]}" -gt 0 ] || exit 0
status=0
# The C++ layer and the Arduino port are C++ down to their headers, so they
# format as C++ even where the suffix says C: uncrustify drops a required
# semicolon after a template-id class head it parses as C. The generated
# bundle header carries C++ namespaces, so it joins them wherever it lives.
mapfile -t c_files < <(printf '%s\n' "${files[@]}" | grep -E '\.(c|cc|h|hh)$' \
  | grep -Ev '^(cpp/|ports/arduino/)' | grep -Ev '^gen/tests/golden/bundle/ros2serial\.h$')
mapfile -t cpp_files < <(printf '%s\n' "${files[@]}" | grep -E '\.(cpp|cxx|hpp|hxx)$')
mapfile -t cxx_headers < <(printf '%s\n' "${files[@]}" \
  | grep -E '^(cpp/|ports/arduino/).*\.(c|h)$|^gen/tests/golden/bundle/ros2serial\.h$')
cpp_files+=("${cxx_headers[@]}")
for lang in C CPP; do
  case $lang in
    C) lang_files=("${c_files[@]}") ;;
    CPP) lang_files=("${cpp_files[@]}") ;;
  esac
  [ "${#lang_files[@]}" -gt 0 ] || continue
  # One process per file: a single uncrustify call over many files can leak
  # parser state from one file into the next, so a batch check can pass a
  # file the same tool flags on its own.
  for f in "${lang_files[@]}"; do
    if [ "$fix" = 1 ]; then
      # Some files settle only on a second pass, so fix to a fixpoint: a file
      # the check still flags after three passes needs a human, not a loop.
      for _ in 1 2 3; do
        uncrustify -c "$cfg" -l "$lang" --replace --no-backup -q "$f" || status=1
        uncrustify -c "$cfg" -l "$lang" --check "$f" >/dev/null 2>&1 && break
        status=1
      done
    fi
    uncrustify -c "$cfg" -l "$lang" --check "$f" || status=1
  done
done
exit $status
