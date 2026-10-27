#!/usr/bin/env bash
# Print the range the PR title check covers: the merge base with the current
# base branch through HEAD. The payload base.sha goes stale when the base
# moves on and is merged in, putting base commits inside the range.
set -uo pipefail
base="origin/${BASE_REF:-master}"
if ! sha=$(git merge-base "$base" HEAD); then
  echo "pr-title-range: no merge base between $base and HEAD" >&2
  exit 1
fi
printf '%s..HEAD\n' "$sha"
