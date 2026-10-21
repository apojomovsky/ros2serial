#!/usr/bin/env bash
# Run a command while holding a lock, so builds sharing one build directory run
# one at a time. Hosts without flock run unlocked rather than failing.
# Usage: with-build-lock.sh <lockfile> <command> [args...]
set -uo pipefail
lock=$1
shift
mkdir -p "$(dirname "$lock")"
if command -v flock >/dev/null 2>&1; then
  exec flock "$lock" "$@"
fi
echo "with-build-lock: flock not found, running unlocked" >&2
exec "$@"
