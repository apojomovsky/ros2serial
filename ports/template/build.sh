#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Compile the port in this directory against the real core with the host
# compiler, then prove the port object defines the contract's symbols.
# Copying this directory to ports/<name>/ and renaming its sources is
# enough: every path below derives from this script's location, so the
# copy checks itself under its own name. Builds go under R2S_BUILD_DIR,
# so the tree stays clean. Usage: build.sh [--out DIR]
set -uo pipefail
port_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
port=$(basename "$port_dir")
port_upper=$(echo "$port" | tr '[:lower:]' '[:upper:]')
root=$(cd "$port_dir/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/ports/$port
self="ports/$port/build.sh"
# One lock for the port build directory, held for the entire run: build
# and test runs of one tree must not write the same objects at once.
lock_var="R2S_${port_upper}_BUILD_LOCKED"
if [ -z "${!lock_var:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env "$lock_var=1" bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$port_dir/build.sh" "$@"
fi
mkdir -p "$build" || exit 1

out=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)
      [ "$#" -ge 2 ] || { echo "$self: '--out' needs a value" >&2; exit 2; }
      out=$2
      shift 2
      ;;
    -h|--help) echo "usage: $self [--out DIR]" >&2; exit 0 ;;
    *) echo "$self: unknown option '$1'" >&2; exit 2 ;;
  esac
done
[ -n "$out" ] || out="$build/obj"
mkdir -p "$out" || exit 1

command -v nm >/dev/null 2>&1 || { echo "$self: nm not found" >&2; exit 1; }
cc=${CC:-gcc}
# The core compiles with its defaults, so the port meets the contract as
# the firmware would see it; test_reset stays out of a non-test build.
"$cc" -std=c99 -Werror -Wall -Wextra -Wconversion -Wshadow \
  -Wstrict-prototypes -Wmissing-prototypes -Wundef \
  -I"$root/core/include" -I"$port_dir" \
  -c "$port_dir/r2s_${port}_port.c" \
  -o "$out/r2s_${port}_port.o" || exit 1
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
[ "$status" -eq 0 ] || { echo "$self: compilation failed" >&2; exit 1; }

# Compiling alone cannot tell a port that forgot a contract function,
# so the defined symbols are listed: each of the four must be there.
missing=""
for sym in r2s_port_read r2s_port_write r2s_port_tx_free r2s_port_tick; do
  nm "$out/r2s_${port}_port.o" | grep -q " T $sym$" || missing="$missing $sym"
done
[ -z "$missing" ] || { echo "$self: no defined symbol for$missing" >&2; exit 1; }
echo "$port build: port and core compile, and the port defines the contract, at $out"
