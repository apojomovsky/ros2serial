#!/usr/bin/env bash
# Run a command while holding a lock, so builds sharing one build directory run
# one at a time. Hosts without flock run unlocked rather than failing.
# Usage: with-build-lock.sh <lockfile> <command> [args...]
#
# The lock is taken by waiting, because a build directory is a resource the
# runs of one tree share: the second run of the same tree waits for the
# first and then uses what it left. A lock that is refused instead is for a
# run that must not happen at all, which the suite does on its own lock.
# Set R2S_LOCK_NOWAIT=1 to refuse rather than wait.
set -uo pipefail
lock=$1
shift
mkdir -p "$(dirname "$lock")"
if ! command -v flock >/dev/null 2>&1; then
  echo "with-build-lock: flock not found, running unlocked" >&2
  exec "$@"
fi

# The lock is taken on a file descriptor, so it is held for as long as this
# shell lives. Taking it and using it is one step, so no run can slip in
# between the check and the hold.
exec 9>"$lock" || {
  echo "with-build-lock: cannot open the lock file ($lock)" >&2
  exit 1
}

if [ "${R2S_LOCK_NOWAIT:-0}" = 1 ]; then
  if ! flock -n 9; then
    echo "with-build-lock: the build directory is busy ($lock); another run holds it" >&2
    echo "with-build-lock: wait for it, or unset R2S_LOCK_NOWAIT" >&2
    exit 75
  fi
else
  flock 9 || {
    echo "with-build-lock: cannot take the lock ($lock)" >&2
    exit 1
  }
fi

"$@"
status=$?
exec 9>&-
exit "$status"
