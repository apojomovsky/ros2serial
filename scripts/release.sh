#!/usr/bin/env bash
# Cut a release: next version from the commits (git-cliff and the [bump] rules
# in cliff.toml), VERSION and CHANGELOG.md updated, one commit. cut-release.yml
# runs this in the dev image, then opens the release PR; the tag follows its
# merge. Only feat, fix, perf, refactor, revert or breaking commits make a
# release: docs, tests, CI and chores alone are nothing to release.
# Usage: release.sh [--dry-run] [--bump auto|patch|minor|major]
set -euo pipefail
usage() { echo "usage: release.sh [--dry-run] [--bump auto|patch|minor|major]"; exit 2; }
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dry=0
bump=auto
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) dry=1 ;;
    --bump)
      [ $# -ge 2 ] || usage
      case $2 in auto|patch|minor|major) bump=$2 ;; *) usage ;; esac
      shift ;;
    *) usage ;;
  esac
  shift
done
command -v git-cliff >/dev/null || { echo "git-cliff not found: run this in the dev image (make shell)"; exit 1; }
cd "$(git rev-parse --show-toplevel)"
cliff=(git-cliff --config "$root/cliff.toml")

last=$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null || true)
range=${last:+$last..}HEAD
subjects=$(git log --format=%s "$range")
releasable='^(feat|fix|perf|refactor|revert)(\(|!|:)|^[a-z]+(\([^)]*\))?!:'
if ! grep -qE "$releasable" <<<"$subjects" && { [ "$bump" = auto ] || [ -z "$subjects" ]; }; then
  echo "nothing to release since ${last:-the start}: no feat, fix, perf, refactor, revert or breaking commits"
  exit 1
fi

if [ "$bump" = auto ]; then
  next=$("${cliff[@]}" --bumped-version)
else
  next=$("${cliff[@]}" --bump "$bump" --bumped-version)
fi
next=${next#v}
echo "next version: $next"
if [ "$dry" = 1 ]; then
  "${cliff[@]}" --unreleased --tag "v$next"
  exit 0
fi
echo "$next" > VERSION
"${cliff[@]}" --tag "v$next" -o CHANGELOG.md
git add VERSION CHANGELOG.md
git commit -q -m "chore(release): v$next"
echo "committed chore(release): v$next"
