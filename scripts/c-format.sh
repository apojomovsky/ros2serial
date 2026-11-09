#!/usr/bin/env bash
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
for lang in C CPP; do
  case $lang in
    C) ext='(c|cc|h|hh)$' ;;
    CPP) ext='(cpp|cxx|hpp|hxx)$' ;;
  esac
  mapfile -t lang_files < <(printf '%s\n' "${files[@]}" | grep -E "\.$ext")
  [ "${#lang_files[@]}" -gt 0 ] || continue
  if [ "$fix" = 1 ]; then
    # Some files settle only on a second pass, so fix to a fixpoint: a file
    # the check still flags after three passes needs a human, not a loop.
    for _ in 1 2 3; do
      uncrustify -c "$cfg" -l "$lang" --replace --no-backup -q "${lang_files[@]}" || status=1
      uncrustify -c "$cfg" -l "$lang" --check "${lang_files[@]}" >/dev/null 2>&1 && break
      status=1
    done
    uncrustify -c "$cfg" -l "$lang" --check "${lang_files[@]}" || status=1
  else
    uncrustify -c "$cfg" -l "$lang" --check "${lang_files[@]}" || status=1
  fi
done
exit $status
