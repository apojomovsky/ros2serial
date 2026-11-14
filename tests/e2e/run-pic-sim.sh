#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# PIC18F4550 end to end in MPLAB SIM (issue #623): build the port, the core
# and the generated pair with the pinned epic-cc, run the firmware in the
# simulator with the handshake vector's host frames scripted into its UART,
# and compare the replies to the vector.
#
# Two images, because neither can do it all. The dev image carries Python
# and generates the pair and the frame lists; the simulator image carries
# epic-cc's clang and the proprietary MPLAB SIM, which is invoked and never
# redistributed. Host-only for that reason, like scripts/run-bigendian.sh.
#
# Usage, from the repository root: make pic-sim
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2

[ -n "${R2S_IN_IMAGE:-}" ] && {
  echo "run-pic-sim.sh: runs on the host, not inside the dev image (it needs docker)" >&2
  exit 1
}
command -v docker >/dev/null 2>&1 || {
  echo "run-pic-sim.sh: docker not found: the PIC sim run needs a Docker daemon" >&2
  exit 1
}

# The simulator image is pinned here, not in ci/versions.env, which only a
# human may touch: this nightly-only image never gates a pull request and
# never measures a budget, exactly as the big-endian and mutation images
# pin their base images in docker/. PIC_SIM_IMAGE overrides it for a local
# experiment; the default is the digest the epic-hal CI image published.
#
# MPLAB SIM is Microchip's proprietary tool. The image is pulled by
# digest and every run of it is a licensed invocation of the tool, the
# same way the epic-hal project runs its own gates; nothing of it is
# copied into this repository or its release artifacts.
image=${PIC_SIM_IMAGE:-ghcr.io/apojomovsky/epic-hal-ci:xc8-v4.00-dfp1.7.162-1.7.171-1.9.258-1.32.471-mplabx6.35@sha256:f2a83758bace48c50b6584feedf5a518d1f83b89832e5c5fb0918943fa6d859b}
case "$image" in
*@sha256:*) ;;
*)
  echo "run-pic-sim.sh: the simulator image is not digest-pinned: '$image'" >&2
  exit 1
  ;;
esac

build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/pic-sim
# One lock for the PIC sim build directory, held for the whole run: a direct
# run and a suite run of one tree must not write the same hex at once.
if [ -z "${R2S_PIC_SIM_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_PIC_SIM_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/tests/e2e/run-pic-sim.sh" "$@"
fi
mkdir -p "$build" || exit 1

vector=$root/spec/vectors/session/pub_sub_handshake.yaml
[ -f "$vector" ] || {
  echo "run-pic-sim.sh: no vector at $vector" >&2
  exit 1
}
work=$build/run
rm -rf "$work"
mkdir -p "$work/gen" || exit 1

# Phase one, in the dev image: the generated pair for the port's own config
# and the frame lists the transcript carries. The cache is mounted at its
# host path, so $work is the same directory in both phases.
gen_cmd="PYTHONPATH=gen python3 -m r2s_gen.cli generate --config ports/epic-hal/r2s.yaml --out '$work/gen' --layout cmake"
if ! make --no-print-directory exec CMD="$gen_cmd" >"$work/gen.log" 2>&1; then
  echo "run-pic-sim.sh: the generator failed on ports/epic-hal/r2s.yaml" >&2
  cat "$work/gen.log" >&2
  exit 1
fi
extract="python3 tools/pic-sim-frames.py '$vector' '$work'"
if ! make --no-print-directory exec CMD="$extract" >"$work/frames.log" 2>&1; then
  echo "run-pic-sim.sh: cannot read the frames out of $vector" >&2
  cat "$work/frames.log" >&2
  exit 1
fi

# The core never includes the generated header, so the limits travel as -D
# flags read from it, the same set the cmake layout passes as definitions.
frame=$(grep -E '^#define R2S_MAX_FRAME[ \t]+[0-9]+u$' "$work/gen/r2s_generated.h" | awk '{ print $3 }' | tr -d 'u')
nest=$(grep -E '^#define R2S_MAX_NEST[ \t]+[0-9]+u$' "$work/gen/r2s_generated.h" | awk '{ print $3 }' | tr -d 'u')
[ -n "$frame" ] && [ -n "$nest" ] || {
  echo "run-pic-sim.sh: the generated header names no frame or nesting limit" >&2
  exit 1
}

if ! docker image inspect "$image" >/dev/null 2>&1; then
  echo "run-pic-sim.sh: pulling the pinned simulator image" >&2
  docker pull "$image" || exit 1
fi

set +u
# The build directory mounts at /build, which is already the run's own
# directory, so the container is told where it is instead of composing it.
exec docker run --rm \
  -v "$root:/src" -v "$build:/build" -w /src \
  -e R2S_PIC_SIM_BUILD=/build \
  -e R2S_MAX_FRAME_DEF="$frame" -e R2S_MAX_NEST_DEF="$nest" \
  "$image" bash /src/tests/e2e/pic-sim-in-container.sh
