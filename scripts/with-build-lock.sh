#!/usr/bin/env bash
# Run a command while holding a lock, so builds sharing one build directory run
# one at a time. Hosts without flock run unlocked rather than failing.
# Usage: with-build-lock.sh <lockfile> <command> [args...]
#
# The lock is not queued: a run that finds the lock held says so and stops,
# because a queue of waiting runs delays every one of them and the last one
# still finds the tree changed under it. Set R2S_LOCK_WAIT=1 to wait instead.
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

if [ "${R2S_LOCK_WAIT:-0}" = 1 ]; then
  flock 9 || {
    echo "with-build-lock: cannot take the lock ($lock)" >&2
    exit 1
  }
else
  if ! flock -n 9; then
    echo "with-build-lock: the build directory is busy ($lock); another run holds it" >&2
    echo "with-build-lock: wait for it, or set R2S_LOCK_WAIT=1 to queue" >&2
    exit 75
  fi
fi

"$@"
status=$?
exec 9>&-
exit "$status"
