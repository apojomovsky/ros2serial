#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Build the RP2040 minimal firmware: resolve the pinned xPack Arm toolchain
# (the image, an override, or a verified fetch into the cache), generate
# the firmware pair from the shared ports/common config, stage the core
# with the Pico glue, link the SDK boot stage at the flash base, then link
# one firmware. The Pico boot stage comes from the pinned pico-sdk tarball,
# verified before anything is extracted from it. Builds go under
# R2S_BUILD_DIR, so the tree stays clean.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/examples
out=$build/rp2040
stage_only=0
# One lock for the examples build directory, held for the whole run: a direct
# run and a suite run of one tree must not write the same ELF at once.
if [ -z "${R2S_RP2040_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_RP2040_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/ports/rp2040/build.sh" "$@"
fi
mkdir -p "$build" || exit 1

while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)
      # A missing value must fail, not loop: shift 2 with one argument
      # leaves the option in place and the parse never advances.
      [ "$#" -ge 2 ] || { echo "ports/rp2040/build.sh: '$1' needs a value" >&2; exit 2; }
      out=$2
      shift 2
      ;;
    --stage-only) stage_only=1; shift ;;
    -h|--help) echo "usage: ports/rp2040/build.sh [--out DIR] [--stage-only]" >&2; exit 0 ;;
    *) echo "ports/rp2040/build.sh: unknown option '$1'" >&2; exit 2 ;;
  esac
done

versions=${R2S_VERSIONS_ENV:-"$root/ci/versions.env"}
arm_ver=$(grep '^ARM_GCC_VERSION=' "$versions" 2>/dev/null | cut -d= -f2)
arm_sha=$(grep '^ARM_GCC_SHA256=' "$versions" 2>/dev/null | cut -d= -f2)
[ -n "$arm_ver" ] && [ -n "$arm_sha" ] || {
  echo "ports/rp2040/build.sh: $versions names no ARM_GCC_VERSION or ARM_GCC_SHA256" >&2
  exit 1
}
sdk_ver=$(grep '^PICO_SDK_VERSION=' "$versions" 2>/dev/null | cut -d= -f2)
sdk_sha=$(grep '^PICO_SDK_SHA256=' "$versions" 2>/dev/null | cut -d= -f2)
[ -n "$sdk_ver" ] && [ -n "$sdk_sha" ] || {
  echo "ports/rp2040/build.sh: $versions names no PICO_SDK_VERSION or PICO_SDK_SHA256" >&2
  exit 1
}

arm_gcc=${R2S_ARM_GCC:-}
if [ -n "$arm_gcc" ]; then
  [ -x "$arm_gcc" ] || {
    echo "ports/rp2040/build.sh: R2S_ARM_GCC names no executable ($arm_gcc): want arm-none-eabi-gcc" >&2
    exit 1
  }
elif command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  arm_gcc=arm-none-eabi-gcc
else
  # A run whose image predates the toolchain fetches the pinned tarball
  # into the cache, the same file the image build downloads. The tarball
  # extracts one version directory beside the next, like the image keeps.
  cache=${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}/xpack-arm
  dir=xpack-arm-none-eabi-gcc-$arm_ver
  file=$dir-linux-x64.tar.gz
  arm_gcc="$cache/$dir/bin/arm-none-eabi-gcc"
  if [ ! -x "$arm_gcc" ]; then
    command -v curl >/dev/null 2>&1 || {
      echo "ports/rp2040/build.sh: no arm-none-eabi-gcc and no downloader" >&2
      exit 1
    }
    mkdir -p "$cache" || exit 1
    url="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v$arm_ver/$file"
    curl -fsSL -o "$cache/$file" "$url" || exit 1
    (cd "$cache" && echo "$arm_sha  $file" | sha256sum -c -) || exit 1
    tar -xz -C "$cache" -f "$cache/$file" || exit 1
    [ -x "$arm_gcc" ] || {
      echo "ports/rp2040/build.sh: $url holds no arm-none-eabi-gcc binary" >&2
      exit 1
    }
  fi
fi

arm_objcopy=${R2S_ARM_OBJCOPY:-}
if [ -z "$arm_objcopy" ]; then
  gcc_dir=$(dirname "$arm_gcc")
  if [ -x "$gcc_dir/arm-none-eabi-objcopy" ]; then
    arm_objcopy="$gcc_dir/arm-none-eabi-objcopy"
  else
    arm_objcopy=arm-none-eabi-objcopy
  fi
fi
command -v "$arm_objcopy" >/dev/null 2>&1 || {
  echo "ports/rp2040/build.sh: no arm-none-eabi-objcopy beside $arm_gcc or on PATH" >&2
  exit 1
}

# A fresh output tree: a renamed linker section must not leave the
# previous link beside the new one for the reporters to measure. The
# empty and root paths are refused before anything is removed.
case "$out" in
  ""|/) echo "ports/rp2040/build.sh: refusing to clear '$out'" >&2; exit 2 ;;
esac
rm -rf "$out"
mkdir -p "$out" || exit 1
# The generated firmware sources: r2s-gen emits the pair for the layout's
# wrapper, and the build copies the pair, ignoring the wrapper.
stage="$out/stage"
gendir="$out/generated"
mkdir -p "$stage" "$gendir" || exit 1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli generate \
  --config "$root/ports/common/r2s.yaml" --out "$gendir" --layout cmake || exit 1
cp "$gendir/r2s_generated.h" "$gendir/r2s_generated.c" "$stage/" || exit 1
mkdir -p "$stage/r2s" || exit 1
cp core/include/r2s/*.h "$stage/r2s/" || exit 1
cp core/src/*.c "$stage/" || exit 1
rm -f "$stage/test_reset.c"
cp ports/common/firmware.c ports/common/r2s_board.h ports/rp2040/r2s_pico.c \
  ports/rp2040/rp2040.ld "$stage/" || exit 1

# The core never includes the generated header, so the limits travel as -D
# flags read from it: the same set the cmake layout passes as definitions.
extras=$(PYTHONPATH="$root/gen" python3 - "$stage/r2s_generated.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "r2s_generated.h")
except (MessageError, OSError, UnicodeDecodeError) as error:
    print(f"r2s-gen: error: {error}", file=sys.stderr)
    raise SystemExit(1)
print(" ".join("-D" + define for define in defines))
EOF
) || exit 1
case "$extras" in
  *R2S_WITH_CODEC*R2S_MAX_FRAME=[0-9]*R2S_MAX_NEST=[0-9]*) ;;
  *) echo "rp2040 build: the extracted flags name no limits ($extras)" >&2; exit 1 ;;
esac
echo "rp2040 build: limits $extras"
[ "$stage_only" = 1 ] && exit 0

cflags="-std=c99 -Os -ffunction-sections -fdata-sections -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wundef"
specs="--specs=nano.specs --specs=nosys.specs"

# The Pico firmware links the SDK boot stage at the flash base, then the
# firmware after it. The SDK tarball below carries only the boot stage in.
sdk_cache=${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}/pico-sdk-$sdk_ver
sdk=${R2S_PICO_SDK:-$sdk_cache/pico-sdk-$sdk_ver}
boot2_src="$sdk/src/rp2040/boot_stage2"
if [ ! -f "$boot2_src/boot2_w25q080.S" ] || [ ! -f "$boot2_src/boot_stage2.ld" ] ||
   [ ! -f "$boot2_src/pad_checksum" ]; then
  command -v curl >/dev/null 2>&1 || {
    echo "ports/rp2040/build.sh: no pico-sdk under $sdk and no downloader" >&2
    exit 1
  }
  mkdir -p "$sdk_cache" || exit 1
  url="https://github.com/raspberrypi/pico-sdk/archive/refs/tags/$sdk_ver.tar.gz"
  curl -fsSL -o "$sdk_cache/pico-sdk-$sdk_ver.tar.gz" "$url" || exit 1
  (cd "$sdk_cache" && echo "$sdk_sha  pico-sdk-$sdk_ver.tar.gz" | sha256sum -c -) || exit 1
  tar -xz -C "$sdk_cache" -f "$sdk_cache/pico-sdk-$sdk_ver.tar.gz" || exit 1
  sdk="$sdk_cache/pico-sdk-$sdk_ver"
  boot2_src="$sdk/src/rp2040/boot_stage2"
fi
[ -f "$boot2_src/boot2_w25q080.S" ] && [ -f "$boot2_src/boot_stage2.ld" ] &&
  [ -f "$boot2_src/pad_checksum" ] || {
    echo "ports/rp2040/build.sh: $sdk holds no boot stage" >&2
    exit 1
  }

pico="$out"
# The boot stage intermediates stay out of the measured tree: like the
# Arduino build directory, anything under a build/ component is skipped.
pico_obj="$pico/build"
mkdir -p "$pico_obj" || exit 1
# The stage source includes pico/asm_helper.S, which includes pico.h but
# uses nothing from it: the SDK's own pico.h drags in the generated board
# config, so the build writes a shim with the stock stringize macros
# instead. The pin keeps the stage source stable under it.
mkdir -p "$pico/include" || exit 1
cat >"$pico/include/pico.h" <<'EOF'
#ifndef _PICO_H
#define _PICO_H
#define __PICO_STRING(x) #x
#define __PICO_XSTRING(x) __PICO_STRING(x)
#define __PICO_CONCAT1(x, y) x ## y
#endif
EOF
# shellcheck disable=SC2086 # the flag strings split on purpose.
"$arm_gcc" -mcpu=cortex-m0plus -mthumb -c "$boot2_src/boot2_w25q080.S" \
  -I "$pico/include" -I "$boot2_src/asminclude" -I "$sdk/src/rp2040/pico_platform/include" \
  -I "$sdk/src/rp2040/hardware_regs/include" \
  -o "$pico_obj/boot2.o" || exit 1
"$arm_gcc" -mcpu=cortex-m0plus -mthumb -nostartfiles -T "$boot2_src/boot_stage2.ld" \
  "$pico_obj/boot2.o" -o "$pico_obj/boot2.elf" || exit 1
"$arm_objcopy" -O binary "$pico_obj/boot2.elf" "$pico_obj/boot2.bin" || exit 1
"$boot2_src/pad_checksum" -s 0xffffffff "$pico_obj/boot2.bin" "$pico_obj/boot2_padded.S" || exit 1
"$arm_gcc" -mcpu=cortex-m0plus -mthumb -c "$pico_obj/boot2_padded.S" -o "$pico_obj/boot2_padded.o" || exit 1
# shellcheck disable=SC2086 # the flag strings split on purpose.
"$arm_gcc" -mcpu=cortex-m0plus -mthumb $cflags $extras -I "$stage" \
  -T "$stage/rp2040.ld" -nostartfiles $specs -Wl,--gc-sections,-Map,"$pico/firmware.map" \
  "$pico_obj/boot2_padded.o" $stage/firmware.c $stage/r2s_pico.c $stage/actions.c \
  $stage/codec.c $stage/crc.c $stage/error.c $stage/framer.c $stage/log.c $stage/params.c \
  $stage/rcobs.c $stage/rx.c $stage/services.c $stage/session.c $stage/time.c \
  $stage/topics.c $stage/tx.c $stage/varint.c $stage/r2s_generated.c \
  -o "$pico/firmware.elf" || exit 1
"$arm_objcopy" -O binary "$pico/firmware.elf" "$pico/firmware.bin" || exit 1
echo "rp2040 build: firmware at $out"
