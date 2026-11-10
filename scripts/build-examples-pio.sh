#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Install-and-build gate for the PlatformIO layout (X7): installs the pinned
# PlatformIO Core and the pinned Atmel AVR platform into a cache directory,
# never into the dev image, stages the platformio layout with the core and
# the Arduino port glue as one library, then builds the pio_hello sketch for
# Uno and Mega with `pio ci`. The manifest carries the frame limits as build
# flags, so the sketch names no defines. Builds and installs go under
# R2S_BUILD_DIR and the cache directory, so the tree stays clean.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/examples
# One lock for the examples build directory, held for the whole run: a direct
# run and a suite run of one tree must not write the same ELF at once.
if [ -z "${R2S_EXAMPLES_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_EXAMPLES_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/scripts/build-examples-pio.sh" "$@"
fi
mkdir -p "$build" || exit 1

boards=()
lib=""
stage_only=""
while [ $# -gt 0 ]; do
  case "$1" in
    --board)
      [ -n "${2:-}" ] || { echo "build-examples-pio: --board needs a value" >&2; exit 1; }
      boards+=("$2")
      shift 2
      ;;
    --board=*)
      boards+=("${1#--board=}")
      shift
      ;;
    --lib)
      [ -n "${2:-}" ] || { echo "build-examples-pio: --lib needs a value" >&2; exit 1; }
      lib="$2"
      shift 2
      ;;
    --lib=*)
      lib="${1#--lib=}"
      shift
      ;;
    --stage-only)
      stage_only=1
      shift
      ;;
    *)
      echo "build-examples-pio: unknown option '$1'" >&2
      exit 1
      ;;
  esac
done
[ "${#boards[@]}" -gt 0 ] || boards=(uno mega)
[ -n "$lib" ] || lib="$build/pio-stage"

# The board names the sketch selects and the PlatformIO board ID it builds.
board_id() {
  case "$1" in
    uno) printf 'uno' ;;
    mega) printf 'megaatmega2560' ;;
    *)
      echo "build-examples-pio: unknown board '$1' (known: uno, mega)" >&2
      return 1
      ;;
  esac
}

versions=${R2S_VERSIONS_ENV:-"$root/ci/versions.env"}
pin=$(grep '^PLATFORMIO_VERSION=' "$versions" 2>/dev/null | cut -d= -f2)
[ -n "$pin" ] || {
  echo "build-examples-pio: $versions names no PLATFORMIO_VERSION" >&2
  exit 1
}
platform_pin=$(grep '^PLATFORMIO_ATMELAVR_VERSION=' "$versions" 2>/dev/null | cut -d= -f2)
[ -n "$platform_pin" ] || {
  echo "build-examples-pio: $versions names no PLATFORMIO_ATMELAVR_VERSION" >&2
  exit 1
}

# Every board name resolves before anything stages or installs, so a
# misspelled board fails fast instead of after the downloads.
ids=()
for board in "${boards[@]}"; do
  ids+=("$(board_id "$board")") || exit 1
done

# The staged library: the generated platformio layout, the core and the
# Arduino port glue. The manifest's build flags reach the compile, so the
# sketch keeps no --build-property flag string.
stage="$lib"
rm -rf "$stage"
mkdir -p "$stage/src/r2s" || exit 1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli generate --layout platformio \
  --config "$root/examples/pio_hello/r2s.yaml" --out "$stage" || exit 1
cp core/include/r2s/*.h "$stage/src/r2s/" || exit 1
cp core/src/*.c "$stage/src/" || exit 1
rm -f "$stage/src/test_reset.c"
cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$stage/src/" || exit 1
# A flag string without the limits would compile a firmware that silently
# ignores every subscription or refuses its own table, so every value must
# be present and non-empty.
extras=$(python3 - "$stage/library.json" <<'EOF'
import json
import sys

try:
    flags = json.load(open(sys.argv[1], encoding='utf-8'))['build']['flags']
except (OSError, ValueError, KeyError, TypeError) as error:
    print(f'build-examples-pio: the staged library.json names no build flags ({error})')
    raise SystemExit(1)
print(' '.join(flags))
EOF
) || exit 1
case "$extras" in
  *R2S_WITH_CODEC*R2S_MAX_FRAME=[0-9]*R2S_MAX_NEST=[0-9]*) ;;
  *)
    echo "build-examples-pio: the staged flags name no limits ($extras)" >&2
    exit 1
    ;;
esac
echo "build-examples-pio flags: $extras"
if [ -n "$stage_only" ]; then
  exit 0
fi

# PlatformIO installs into the cache directory, never into the image: the
# first run downloads Core and the AVR platform, later runs reuse them, and
# CI restores the directory from its cache. R2S_PIO_BIN points the gate at
# a binary under test instead.
data_dir=${R2S_PIO_DATA_DIR:-${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}/platformio}
if [ -n "${R2S_PIO_BIN:-}" ]; then
  pio="$R2S_PIO_BIN"
else
  venv="$data_dir/venv"
  if [ ! -x "$venv/bin/pio" ] || ! "$venv/bin/pio" --version 2>/dev/null | grep -q "version $pin"; then
    rm -rf "$venv"
    python3 -m venv "$venv" || exit 1
    "$venv/bin/pip" install -q "platformio==$pin" || exit 1
  fi
  pio="$venv/bin/pio"
fi
export PLATFORMIO_CORE_DIR="$data_dir/core"
mkdir -p "$PLATFORMIO_CORE_DIR" || exit 1
"$pio" pkg install --global --platform "atmelavr@$platform_pin" || exit 1

status=0
out="$build/pio"
mkdir -p "$out" || exit 1
# `pio ci` assembles a project under --build-dir and, with --keep-build-dir,
# leaves every board's firmware there; without it the project is temporary
# and the gate could not name the artifact it proved.
# shellcheck disable=SC2207 # one --board flag per resolved id.
board_flags=($(printf -- '--board=%s ' "${ids[@]}"))
# A previous run's objects would skip the compile and with it the proof,
# so the build starts clean; verbose, so the log names the compile flags
# the gate then checks.
rm -rf "$out/.pio" "$out/build.log"
(cd "$out" && "$pio" ci --verbose --lib="$stage" "${board_flags[@]}" --build-dir="$out" \
  --keep-build-dir "$root/examples/pio_hello/pio_hello.ino" 2>&1 | tee "$out/build.log") || status=1
for index in "${!boards[@]}"; do
  if [ ! -f "$out/.pio/build/${ids[$index]}/firmware.elf" ]; then
    echo "build-examples-pio (${boards[$index]}): the build left no firmware.elf" >&2
    status=1
  fi
done
# The core headers fall back to default limits when a define never arrives,
# which still links: every staged flag must appear in a compile line.
# shellcheck disable=SC2086 # the flags are -D words by construction.
for flag in $extras; do
  grep -q -F -- "$flag" "$out/build.log" || {
    echo "build-examples-pio: the firmware build never compiled $flag" >&2
    status=1
  }
done

exit "$status"
