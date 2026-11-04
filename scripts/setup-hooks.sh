#!/usr/bin/env bash
# Install git hooks and register this clone's merge drivers.
# Usage: setup-hooks.sh [<repo>]; the repository defaults to the current directory.
# The generated-files driver is always registered; mergiraf only when it is
# on PATH, since git falls back to its line merge without it.
set -euo pipefail
repo=${1:-$PWD}
git -C "$repo" config core.hooksPath .githooks && echo "git hooks installed (core.hooksPath = .githooks)"
git -C "$repo" config merge.ours.driver true
git -C "$repo" config merge.ours.name "keep our generated files; CI regenerates"
if command -v mergiraf >/dev/null 2>&1; then
  git -C "$repo" config merge.mergiraf.name mergiraf
  git -C "$repo" config merge.mergiraf.driver "mergiraf merge --git %O %A %B -s %S -x %X -y %Y -p %P -l %L"
  echo "mergiraf merge driver registered"
else
  echo "mergiraf not on PATH: line merge applies until it is installed (optional)"
fi
