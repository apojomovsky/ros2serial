#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Sourced, not run: resolves the pinned xPack Arm toolchain the STM32
# and RP2040 firmware builds link with. The caller sets root and self;
# this sets arm_gcc from the image, an override, or a verified fetch
# of the pinned tarball into the cache.
versions=${R2S_VERSIONS_ENV:-"$root/ci/versions.env"}
arm_ver=$(grep '^ARM_GCC_VERSION=' "$versions" 2>/dev/null | cut -d= -f2)
arm_sha=$(grep '^ARM_GCC_SHA256=' "$versions" 2>/dev/null | cut -d= -f2)
[ -n "$arm_ver" ] && [ -n "$arm_sha" ] || {
  echo "$self: $versions names no ARM_GCC_VERSION or ARM_GCC_SHA256" >&2
  exit 1
}

arm_gcc=${R2S_ARM_GCC:-}
if [ -n "$arm_gcc" ]; then
  [ -x "$arm_gcc" ] || {
    echo "$self: R2S_ARM_GCC names no executable ($arm_gcc): want arm-none-eabi-gcc" >&2
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
      echo "$self: no arm-none-eabi-gcc and no downloader" >&2
      exit 1
    }
    mkdir -p "$cache" || exit 1
    url="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v$arm_ver/$file"
    curl -fsSL -o "$cache/$file" "$url" || exit 1
    (cd "$cache" && echo "$arm_sha  $file" | sha256sum -c -) || exit 1
    tar -xz -C "$cache" -f "$cache/$file" || exit 1
    [ -x "$arm_gcc" ] || {
      echo "$self: $url holds no arm-none-eabi-gcc binary" >&2
      exit 1
    }
  fi
fi
