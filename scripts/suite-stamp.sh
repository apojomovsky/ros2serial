#!/usr/bin/env bash
# One pass, one stamp: the key names the tree, the suite definition and the
# scope set, so a takeoff that passed makes the following push skip the suite.
# A narrower scope set is a different key, so a scoped pass never skips a run
# the wider scope still owes. Usage: suite-stamp.sh key "<scopes>".
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
[ "${1:-}" = key ] || { echo "usage: suite-stamp.sh key \"<scopes>\"" >&2; exit 2; }
scopes=$(printf '%s' "${2:-}" | tr ' ' '\n' | grep -v '^$' | sort -u | tr '\n' ' ')
tree=$(git rev-parse 'HEAD^{tree}') || exit 1
suite_def=$(cat "$root"/.githooks/* "$root/scripts/ci-test.sh" "$root/scripts/suite-lock.sh" \
  "$root/scripts/clean-git-env.sh" "$root/scripts/with-build-lock.sh" \
  "$root/scripts/pre-pr-check.sh" "$root/scripts/scopes_for_diff.py" \
  "$root/scripts/suite-stamp.sh" "$root/Makefile" | sha256sum | cut -c1-16) || exit 1
echo "$tree $suite_def $scopes"
