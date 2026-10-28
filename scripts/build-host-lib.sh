#!/usr/bin/env bash
# Builds libr2s_host.so for the Python model-based session tests (T3): the
# core plus the fake port and the handwritten test firmware, compiled with
# R2S_TESTING as the host unit builds are. A second library builds the same
# sources with R2S_TEST_INJECT_READY_ZERO_TOKEN, which accepts a READY token
# of 0; the injected-bug test shows the model catches that breach, and CI
# rebuilds the pair from source on every run, so the demonstration is not a
# one-off local edit.
# Usage: build-host-lib.sh [out-dir] (default: $R2S_BUILD_DIR/host)
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
out=${1:-${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/host}
mkdir -p "$out" || exit 1

flags=(-std=c99 -shared -fPIC -O2 -Wall -Wextra -Werror)
defines=(-DR2S_TESTING -DR2S_MAX_FRAME=512 -DR2S_WITH_CODEC -DR2S_MAX_NEST=2 -DR2S_WITH_LOG)
includes=(-I core/include -I core/tests/support -I core/host)
sources=(core/src/*.c core/tests/support/fake_port.c core/tests/support/bytes.c
    core/tests/support/handwritten.c core/tests/support/handwritten_firmware.c core/host/host_shim.c)

cc=${CC:-cc}
"$cc" "${flags[@]}" "${defines[@]}" "${includes[@]}" "${sources[@]}" -o "$out/libr2s_host.so" || exit 1
"$cc" "${flags[@]}" "${defines[@]}" -DR2S_TEST_INJECT_READY_ZERO_TOKEN=1 "${includes[@]}" "${sources[@]}" \
    -o "$out/libr2s_host_injected.so" || exit 1
echo "libr2s_host.so and libr2s_host_injected.so in $out"
