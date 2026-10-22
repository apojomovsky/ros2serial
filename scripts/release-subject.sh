#!/usr/bin/env bash
# Is the commit that last changed VERSION a release commit?
# The tag job refuses to tag unless it is, so this is the check it runs.
# A squash puts the PR number after the subject, so the subject is matched
# by its prefix, not in full. The version is matched literally, so a version
# with a character the pattern would read is not a match.
# Usage: release-subject.sh <version> <subject>
#   exit 0  the subject is the release commit for that version
#   exit 1  it is not
set -uo pipefail
version=$1
subject=$2
case $subject in
  "chore(release): v$version" | "chore(release): v$version "*) exit 0 ;;
esac
exit 1
