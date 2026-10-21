#!/usr/bin/env bash
# Print the dev image tag: a hash of everything the image is built from, so
# `make image` is a no-op when nothing changed and a new tag when anything did.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
hash=$(cat "$root/Dockerfile" "$root/ci/versions.env" | sha256sum | cut -c1-12)
echo "ros2serial-dev:$hash"
