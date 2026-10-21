#!/usr/bin/env bash
# Cut a release locally: next version from the commits (git-cliff and the [bump]
# rules in cliff.toml), VERSION and CHANGELOG.md updated, one commit. cut-release.yml
# runs this, then opens the release PR; the tag follows its merge.
# Usage: release.sh [--dry-run] [--bump auto|patch|minor|major]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dry=0
bump=auto
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) dry=1 ;;
    --bump) bump=$2; shift ;;
    *) echo "usage: release.sh [--dry-run] [--bump auto|patch|minor|major]"; exit 2 ;;
  esac
  shift
done
cliff=(git-cliff --config "$root/cliff.toml")
if [ "$bump" = auto ]; then
  next=$("${cliff[@]}" --bumped-version 2>/dev/null)
else
  next=$("${cliff[@]}" --bump "$bump" --bumped-version 2>/dev/null)
fi
next=${next#v}
echo "next version: $next"
if [ "$dry" = 1 ]; then
  "${cliff[@]}" --unreleased --tag "v$next" 2>/dev/null
  exit 0
fi
echo "$next" > VERSION
"${cliff[@]}" --tag "v$next" -o CHANGELOG.md 2>/dev/null
git add VERSION CHANGELOG.md
git commit -q -m "chore(release): v$next"
echo "committed chore(release): v$next"
