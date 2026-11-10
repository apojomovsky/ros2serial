#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# make lint-docs: parse every fenced mermaid block in the tracked markdown
# with mermaid's own parser, inside the pinned node image. The dev image
# gains no Node, so this runs on the host through docker run and needs a
# Docker daemon; inside the dev image it stops with a precise message.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() {
  echo "lint-docs: $*" >&2
  exit 1
}

[ -n "${R2S_IN_IMAGE:-}" ] && fail "runs on the host, not inside the dev image (it needs docker)"

common=$(git -C "$root" rev-parse --git-common-dir 2>/dev/null) || fail "not inside a git repository"
case "$common" in /*) ;; *) common="$root/$common" ;; esac
mount=$(cd "$common/.." && pwd) || fail "cannot locate the repository root"

image=$(sed -n 's/^NODE_IMAGE=//p' "$root/ci/versions.env")
[[ "$image" =~ ^node:22-slim@sha256:[0-9a-f]{64}$ ]] || fail "ci/versions.env does not pin NODE_IMAGE"

cache=${XDG_CACHE_HOME:-$HOME/.cache}/ros2serial/npm
mkdir -p "$cache" || fail "cannot create $cache"

# The node image carries no git, so the host lists the tracked markdown
# and passes it in; each entry stays its own argument into the container.
mapfile -t files < <(git -C "$root" ls-files '*.md') || fail "cannot list tracked markdown"
[ "${#files[@]}" -gt 0 ] || {
  echo "mermaid: no tracked markdown files"
  exit 0
}

docker run --rm --user "$(id -u):$(id -g)" \
  -v "$mount:$mount" -w "$root/tools/mermaid-check" \
  -v "$cache:/npm-cache" -e npm_config_cache=/npm-cache \
  "$image" sh -c 'npm ci --no-audit --no-fund && node --test && node check.mjs "$@"' sh "${files[@]}"
