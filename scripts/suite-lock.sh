#!/usr/bin/env bash
# The lock a suite run of a tree takes: two runs of one tree must agree on it,
# and two trees must never share one. A tree the dev image mounts keeps the
# same path in every container, so the shared cache can key it; a tree made
# inside one container (pytest's copy) has a path no other container sees, so
# its lock stays in that container's temporary directory instead.
# Usage: suite-lock.sh <tree-root>
set -uo pipefail
root=$1

cache=${R2S_CACHE_DIR:-}
if [ -z "$cache" ] && [ -n "${R2S_BUILD_DIR:-}" ]; then
  cache=$(dirname "$R2S_BUILD_DIR")
fi
cache=${cache:-$HOME/.cache/ros2serial}

lock_root=$cache
if [ -n "${R2S_HOST_MOUNT:-}" ]; then
  case $root in
    "$R2S_HOST_MOUNT" | "$R2S_HOST_MOUNT"/*) ;;
    *) lock_root=${TMPDIR:-/tmp}/r2s-suite-lock ;;
  esac
fi

# A "/" to "-" swap alone maps two different trees to one name.
key=$(printf '%s' "$root" | sha256sum) || exit 1
key=${key%% *}
printf '%s\n' "$lock_root/$key/suite.lock"
