#!/usr/bin/env bash
# Is the commit that last changed VERSION a release commit?
# The tag job refuses to tag unless it is, so this is the check it runs.
# A squash puts the PR number after the subject, so the subject is matched
# by its prefix, not in full.
# Usage: release-subject.sh <version> <subject>
#   exit 0  the subject is the release commit for that version
#   exit 1  it is not
set -uo pipefail
version=$1
subject=$2
printf '%s\n' "$subject" | grep -qE "^chore\(release\): v${version}($| )"
