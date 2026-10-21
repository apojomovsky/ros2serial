#!/usr/bin/env bash
# Refuse builds from the main clone while worktrees exist: agents building in
# the same directory share one build dir and corrupt each other's builds.
# ALLOW_MAIN_CLONE=1 overrides for one-offs.
set -uo pipefail
[ "${ALLOW_MAIN_CLONE:-}" = 1 ] && exit 0
git_dir=$(git rev-parse --absolute-git-dir 2>/dev/null) || exit 0
common=$(cd "$(git rev-parse --git-common-dir)" && pwd)
[ "$git_dir" != "$common" ] && exit 0
top=$(git rev-parse --show-toplevel)
if [ -d "$top/.worktrees" ] && [ -n "$(ls -A "$top/.worktrees" 2>/dev/null)" ]; then
  echo "refusing to build in the main clone while .worktrees/ has worktrees: work from .worktrees/<name>"
  echo "(ALLOW_MAIN_CLONE=1 overrides for a one-off)"
  exit 1
fi
exit 0
