#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Compile the template port against the real core with the host compiler.
# This proves a copied port still matches the contract after its renames,
# before its own test runs. Builds go under R2S_BUILD_DIR, so the tree
# stays clean. Usage: build.sh [--out DIR]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/ports/template

out=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)
      [ "$#" -ge 2 ] || { echo "ports/template/build.sh: '--out' needs a value" >&2; exit 2; }
      out=$2
      shift 2
      ;;
    -h|--help) echo "usage: ports/template/build.sh [--out DIR]" >&2; exit 0 ;;
    *) echo "ports/template/build.sh: unknown option '$1'" >&2; exit 2 ;;
  esac
done
[ -n "$out" ] || out="$build/obj"
mkdir -p "$out" || exit 1

cc=${CC:-gcc}
# The core compiles with its defaults, so the port meets the contract as
# the firmware would see it; test_reset stays out of a non-test build.
"$cc" -std=c99 -Werror -Wall -Wextra -Wconversion -Wshadow \
  -Wstrict-prototypes -Wmissing-prototypes -Wundef \
  -I"$root/core/include" -I"$root/ports/template" \
  -c "$root/ports/template/r2s_template_port.c" \
  -o "$out/r2s_template_port.o" || exit 1
status=0
for src in "$root"/core/src/*.c; do
  case "$src" in
    */test_reset.c) continue ;;
  esac
  name=$(basename "$src" .c)
  # shellcheck disable=SC2086 # no extra flags to split here.
  "$cc" -std=c99 -Werror -Wall -Wextra -Wconversion -Wshadow \
    -Wstrict-prototypes -Wmissing-prototypes -Wundef \
    -I"$root/core/include" \
    -c "$src" -o "$out/$name.o" || status=1
done
[ "$status" -eq 0 ] || { echo "template build: compilation failed" >&2; exit 1; }
echo "template build: port and core compile at $out"
