#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Build the bridge libFuzzer targets and run each over its seed corpus, then
# fuzz it. Usage: fuzz-bridge.sh; env TARGET picks one target
# (bridge_session, bridge_converter), default both; env R2S_FUZZ_SECONDS
# sets the fuzz time per target, default 60; 0 only runs the seeds.
# R2S_FUZZ_ARTIFACTS_DIR keeps whatever libFuzzer adds beyond the copied
# seeds under that directory for upload. Exits 1 on any build, seed or crash.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
command -v clang++ >/dev/null 2>&1 || { echo "clang++ not found: run it in the dev image" >&2; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "cmake not found: run it in the dev image" >&2; exit 1; }
if [ -z "${AMENT_PREFIX_PATH:-}" ]; then
  [ -n "${ROS_DISTRO:-}" ] || { echo "ROS_DISTRO is not set: run it in a ROS container" >&2; exit 1; }
  [ -f "/opt/ros/$ROS_DISTRO/setup.bash" ] || { echo "no ROS setup at /opt/ros/$ROS_DISTRO" >&2; exit 1; }
  # shellcheck source=/dev/null
  set +u
  source "/opt/ros/$ROS_DISTRO/setup.bash"
  set -u
fi

# A stale seed next to a changed vector must fail here, not fuzz silently.
python3 tools/fuzz_corpus.py --check || exit 1

build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/fuzz-bridge
targets=(${TARGET:-bridge_session bridge_converter})
seconds=${R2S_FUZZ_SECONDS:-60}
artifacts=${R2S_FUZZ_ARTIFACTS_DIR:-}
export LSAN_OPTIONS="suppressions=$root/bridge/lsan_suppressions.txt"
export UBSAN_OPTIONS="print_stacktrace=1"

mkdir -p "$build" || exit 1
cmake -S bridge/r2s_bridge -B "$build" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-O1 -g" -DCMAKE_CXX_FLAGS="-O1 -g" \
  -DR2S_BRIDGE_FUZZ=ON -DBUILD_TESTING=OFF >/dev/null || {
  echo "-- fuzz-bridge: configure FAILED"
  exit 1
}
cmake --build "$build" --target fuzz_bridge_session fuzz_bridge_converter -j"$(nproc)" || {
  echo "-- fuzz-bridge: build FAILED"
  exit 1
}

failed=()
for target in "${targets[@]}"; do
  case "$target" in
    bridge_session | bridge_converter) ;;
    *)
      echo "unknown bridge fuzz target '$target'"
      failed+=("$target")
      continue
      ;;
  esac
  binary="$build/fuzz_$target"
  corpus="bridge/r2s_bridge/fuzz/corpus/$target"
  echo "-- fuzz: $target"
  work=$(mktemp -d) || exit 1
  cp "$corpus"/* "$work"/
  ok=1
  if [ "$seconds" = 0 ]; then
    "$binary" -runs=0 -timeout=30 -artifact_prefix="$work/" "$work" || {
      echo "-- fuzz: $target seed FAILED"
      failed+=("$target")
      ok=0
    }
  else
    "$binary" -max_total_time="$seconds" -timeout=30 -print_final_stats=1 -artifact_prefix="$work/" "$work" || {
      echo "-- fuzz: $target fuzz FAILED"
      failed+=("$target")
      ok=0
    }
  fi
  if [ -n "$artifacts" ]; then
    mkdir -p "$artifacts/$target" || exit 1
    for entry in "$work"/*; do
      name=${entry##*/}
      [ -e "$corpus/$name" ] || cp "$entry" "$artifacts/$target/$name"
    done
    kept=$(ls "$artifacts/$target" | wc -l)
    echo "-- fuzz: $target kept $kept new inputs in $artifacts/$target"
  fi
  rm -rf "$work"
  [ "$ok" = 1 ] && echo "-- fuzz: $target ok"
done
if [ "${#failed[@]}" -gt 0 ]; then
  echo "bridge fuzz targets failed: ${failed[*]}"
  exit 1
fi
