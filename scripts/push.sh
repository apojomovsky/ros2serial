#!/usr/bin/env bash
# Push the current branch with the hooks, then confirm the remote ref matches
# HEAD. Exit codes: 0 landed, 1 did not land, 2 usage, 3 remote unreachable.
# A failed push is retried once; the pre-push suite stamp makes the retry cheap.
set -uo pipefail
for arg in "$@"; do
  if [ "$arg" = "--no-verify" ]; then
    echo "push.sh: --no-verify is forbidden (AGENTS.md, Ground rules)"
    exit 2
  fi
done
branch=$(git branch --show-current)
[ -n "$branch" ] || { echo "push.sh: not on a branch"; exit 2; }
git push -u origin "$branch" "$@" || { sleep 2; git push -u origin "$branch" "$@"; }
remote=$(git ls-remote origin "refs/heads/$branch") || { echo "push.sh: origin unreachable"; exit 3; }
if [ "${remote%%[[:space:]]*}" = "$(git rev-parse HEAD)" ]; then
  echo "push.sh: origin/$branch is at HEAD"
  exit 0
fi
echo "push.sh: origin/$branch does not match HEAD; the push did not land"
exit 1
