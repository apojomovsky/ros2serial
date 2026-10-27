#!/usr/bin/env bash
# Print the range the PR title check covers: the merge base with the current
# base branch through HEAD. The payload base.sha goes stale when the base
# moves on and is merged in, putting base commits inside the range.
set -uo pipefail
base="origin/${BASE_REF:-master}"
printf '%s..HEAD\n' "$(git merge-base "$base" HEAD)"
