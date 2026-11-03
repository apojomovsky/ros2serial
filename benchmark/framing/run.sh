#!/usr/bin/env bash
# P9 framing benchmark: one firmware per framer on Uno and Mega under simavr.
# Builds go under R2S_BUILD_DIR, so the tree stays clean. Needs the dev image
# (avr-gcc, simavr, pyserial): run through make exec or from make shell.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/benchmark/framing
if [ -z "${R2S_BM_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_BM_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/benchmark/framing/run.sh" "$@"
fi
mkdir -p "$build" || exit 1
[ "$#" -eq 0 ] || { echo "usage: benchmark/framing/run.sh" >&2; exit 2; }

bash tests/harness/simavr/run.sh --build-only || exit 1
sim_build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/harness/simavr
python3 benchmark/framing/run_benchmark.py --build-dir "$build" --harness-bin "$sim_build/r2s_simavr"
