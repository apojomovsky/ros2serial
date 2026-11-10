#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The nightly big-endian job's entry point: build docker/bigendian and run
# the portable test scope inside it, with the core cross-built for s390x and
# tested under qemu-s390x. Host-only: the dev image carries no Docker daemon,
# so this refuses inside it with a precise message, as lint-docs.sh does.
# The Python pins mirror ci/versions.env, so the reference tests meet the
# same dependencies the pull request suite uses; the Dockerfile defaults
# match for direct docker builds. Usage, from the repository root:
#   task bigendian
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2

[ -n "${R2S_IN_IMAGE:-}" ] && {
  echo "run-bigendian.sh: runs on the host, not inside the dev image (it needs docker)" >&2
  exit 1
}
command -v docker >/dev/null 2>&1 || {
  echo "run-bigendian.sh: docker not found: the big-endian job needs a Docker daemon" >&2
  exit 1
}

pin() { sed -n "s/^$1=//p" "$root/ci/versions.env" | tail -1; }
image=ros2serial-bigendian:local
docker build -f "$root/docker/bigendian/Dockerfile" -t "$image" \
  --build-arg "PYTEST_VERSION=$(pin PYTEST_VERSION)" \
  --build-arg "HYPOTHESIS_VERSION=$(pin HYPOTHESIS_VERSION)" \
  --build-arg "PYYAML_VERSION=$(pin PYYAML_VERSION)" \
  --build-arg "JSONSCHEMA_VERSION=$(pin JSONSCHEMA_VERSION)" \
  "$root" || exit 1
docker run --rm -v "$root:$root" -w "$root" "$image" bash scripts/bigendian-suite.sh
