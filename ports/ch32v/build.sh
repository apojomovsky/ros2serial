#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Build the minimal CH32V003 firmware: generate the static pair, compile
# the core, the port glue and the example with the pinned RISC-V toolchain,
# and link against the V003 memory map. Builds go under R2S_BUILD_DIR, so
# the tree stays clean. Usage: build.sh [--out DIR]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/ch32v

out=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)
      [ "$#" -ge 2 ] || { echo "ports/ch32v/build.sh: '--out' needs a value" >&2; exit 2; }
      out=$2
      shift 2
      ;;
    -h|--help) echo "usage: ports/ch32v/build.sh [--out DIR]" >&2; exit 0 ;;
    *) echo "ports/ch32v/build.sh: unknown option '$1'" >&2; exit 2 ;;
  esac
done
[ -n "$out" ] || out="$build/ch32v_minimal"

command -v riscv-none-elf-gcc >/dev/null 2>&1 || {
  echo "ports/ch32v/build.sh: riscv-none-elf-gcc not found; the dev image carries the toolchain pinned in ci/versions.env" >&2
  exit 1
}
pin=$(grep '^RISCV_XPACK_VERSION=' "$root/ci/versions.env" | cut -d= -f2)
[ -n "$pin" ] || {
  echo "ports/ch32v/build.sh: ci/versions.env names no RISCV_XPACK_VERSION" >&2
  exit 1
}
echo "ch32v build: toolchain riscv-none-elf-gcc ($pin)"

# The static pair for the example config, emitted at build time like the
# Arduino bundle: no generated sources are checked in for this firmware.
gen="$out/gen"
rm -rf "$gen"
mkdir -p "$gen" || exit 1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli generate \
  --config "$root/examples/ch32v_minimal/r2s.yaml" \
  --out "$gen" --layout cmake || exit 1

# The core never includes the generated header, so the limits travel as -D
# flags read from it, the same set the cmake layout passes as definitions.
frame=$(grep -E '^#define R2S_MAX_FRAME[ \t]+[0-9]+u$' "$gen/r2s_generated.h" | awk '{ print $3 }' | tr -d 'u')
nest=$(grep -E '^#define R2S_MAX_NEST[ \t]+[0-9]+u$' "$gen/r2s_generated.h" | awk '{ print $3 }' | tr -d 'u')
[ -n "$frame" ] && [ -n "$nest" ] || {
  echo "ch32v build: the generated header names no frame or nesting limit" >&2
  exit 1
}
defines="-DR2S_WITH_CODEC -DR2S_MAX_FRAME=$frame -DR2S_MAX_NEST=$nest"
if grep -E '^#define R2S_TOPIC_MAX_COUNT[ \t]+[0-9]+u$' "$gen/r2s_generated.h" >/dev/null; then
  count=$(grep -E '^#define R2S_TOPIC_MAX_COUNT[ \t]+[0-9]+u$' "$gen/r2s_generated.h" | awk '{ print $3 }' | tr -d 'u')
  defines="$defines -DR2S_TOPIC_MAX_COUNT=$count"
fi
echo "ch32v build: limits $defines"

# The QingKe V2A is RV32EC: embedded with compressed instructions.
arch="-march=rv32ec -mabi=ilp32e"
common=(-std=c99 -Os $arch -ffunction-sections -fdata-sections -fstack-usage
  -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes -Wundef)
includes=(-I "$root/core/include" -I "$root/ports/ch32v" -I "$gen")
obj="$out/obj"
rm -rf "$obj"
mkdir -p "$obj" || exit 1
status=0
compile() {
  local src=$1
  local name
  name=$(basename "$src" .c)
  # shellcheck disable=SC2086 # defines and arch hold several flags.
  riscv-none-elf-gcc "${common[@]}" "${includes[@]}" $defines -c "$src" -o "$obj/$name.o" || status=1
}
mapfile -t sources < <(ls "$root"/core/src/*.c | grep -v test_reset.c)
for src in "${sources[@]}" "$gen/r2s_generated.c" \
  "$root/ports/ch32v/r2s_ch32v_port.c" "$root/examples/ch32v_minimal/main.c"; do
  compile "$src"
done
[ "$status" -eq 0 ] || { echo "ch32v build: compilation failed" >&2; exit 1; }

# The startup file is assembly, so it assembles apart from the C units.
riscv-none-elf-gcc $arch -c "$root/ports/ch32v/start.S" -o "$obj/start.o" || exit 1

elf="$out/ch32v_minimal.elf"
mapfile -t objects < <(ls "$obj"/*.o)
# shellcheck disable=SC2086 # the architecture holds two flags.
riscv-none-elf-gcc $arch -nostartfiles -T "$root/ports/ch32v/ch32v003.ld" \
  -Wl,--gc-sections -Wl,-Map,"$out/ch32v_minimal.map" \
  "${objects[@]}" -o "$elf" || exit 1
[ -f "$elf" ] || { echo "ch32v build: no firmware at '$elf'" >&2; exit 1; }
riscv-none-elf-size "$elf" || exit 1

# The static firmware links the generated receive buffer and never the
# runtime topic table, which only the runtime registration path carries.
elf_has_symbol() {
  local want=$1
  local symbols

  symbols=$(riscv-none-elf-nm "$elf" 2>/dev/null || true)
  case $symbols in
  *"$want"*) return 0 ;;
  *) return 1 ;;
  esac
}
if ! elf_has_symbol r2s_rx_buf; then
  echo "ch32v build: the firmware links no receive buffer" >&2
  exit 1
fi
if elf_has_symbol r2s_runtime_topics; then
  echo "ch32v build: the firmware links the runtime topic table" >&2
  exit 1
fi
echo "ch32v build: firmware at $elf"
