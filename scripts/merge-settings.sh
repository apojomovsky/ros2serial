#!/usr/bin/env bash
# Does the repository's merge settings match the rule in ADR-003?
# Squash alone, the PR title as the landed subject, and no message.
# Usage: merge-settings.sh [owner/name]
#   exit 0  the settings match
#   exit 1  they do not, or cannot be read
#   exit 75  gh is not available, so the settings cannot be read
set -uo pipefail
if ! command -v gh >/dev/null 2>&1; then
  echo "merge-settings: gh is not installed, so the settings cannot be read" >&2
  exit 75
fi
repo=${1:-$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)}
if [ -z "$repo" ]; then
  echo "merge-settings: no repository given and gh cannot find one" >&2
  exit 75
fi
settings=$(gh api "repos/$repo" --jq '[
  .allow_squash_merge, .allow_merge_commit, .allow_rebase_merge,
  .squash_merge_commit_title, .squash_merge_commit_message] | @tsv' 2>/dev/null)
if [ -z "$settings" ]; then
  echo "merge-settings: cannot read the settings of $repo" >&2
  exit 1
fi
read -r squash merge rebase title message <<<"$settings"
problems=""
[ "$squash" = true ] || problems="$problems a squash merge is not allowed"
[ "$merge" = false ] || problems="$problems a merge commit is allowed"
[ "$rebase" = false ] || problems="$problems a rebase merge is allowed"
[ "$title" = PR_TITLE ] || problems="$problems the squash title is $title, not PR_TITLE"
[ "$message" = BLANK ] || problems="$problems the squash message is $message, not BLANK"
if [ -n "$problems" ]; then
  echo "the merge settings of $repo do not match ADR-003:$problems" >&2
  exit 1
fi
exit 0
