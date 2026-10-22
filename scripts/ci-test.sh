#!/usr/bin/env bash
# The single suite script: CI runs exactly this, and so does `make test`.
# Scope with one or more of LINT=1 SCRIPTS=1 COMMITS=1 CORE=1 GEN=1 BRIDGE=1
# E2E=<backend>; no scope runs everything. COMMITS checks COMMIT_RANGE
# (default origin/master..HEAD) and PR_TITLE when set, then runs the prose gate
# on the same range. Exits non-zero if any section fails.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/clean-git-env.sh
source "$root/scripts/clean-git-env.sh"
cd "$root" || exit 2

# One suite at a time per repository. A second run would wait on the same
# lock and then test a tree the first run had already changed, so it says so
# and stops instead. The lock lives in the cache, under the repository's
# path, so it is scoped to this repository the way the build directory is.
cache=${R2S_CACHE_DIR:-}
if [ -z "$cache" ] && [ -n "${R2S_BUILD_DIR:-}" ]; then
  cache=$(dirname "$R2S_BUILD_DIR")
fi
cache=${cache:-$HOME/.cache/ros2serial}
# The lock path names the tree the run tests, so a run of a copy of the tree
# takes its own lock and never collides with the run of the tree it was
# copied from. A caller that runs the suite on a tree of its own choosing
# names that tree in R2S_SUITE_LOCK_TREE and the lock in R2S_SUITE_LOCK.
suite_lock=$cache/$(echo "$root" | tr / -)/suite.lock
if [ -n "${R2S_SUITE_LOCK:-}" ] && [ "${R2S_SUITE_LOCK_TREE:-}" = "$root" ]; then
  suite_lock=$R2S_SUITE_LOCK
fi
mkdir -p "$(dirname "$suite_lock")" 2>/dev/null || {
  echo "the suite lock directory cannot be created ($suite_lock)" >&2
  echo "set R2S_CACHE_DIR to a writable directory" >&2
  exit 1
}
# The lock is taken here and held for the whole run, so a second suite run
# is refused. The script does not re-exec itself to take it: the suite runs
# this script, so a self re-exec would nest without end.
exec 9>"$suite_lock" || {
  echo "the suite lock cannot be opened ($suite_lock)" >&2
  exit 1
}
if [ "${R2S_SUITE_LOCK_WAIT:-0}" = 1 ]; then
  flock 9 || {
    echo "the suite lock cannot be taken ($suite_lock)" >&2
    exit 1
  }
elif ! flock -n 9; then
  echo "the suite is already running for this repository ($suite_lock)" >&2
  echo "wait for it, or set R2S_SUITE_LOCK_WAIT=1 to queue" >&2
  exit 75
fi

scoped=0
for v in LINT SCRIPTS COMMITS CORE GEN BRIDGE E2E; do [ -n "${!v:-}" ] && scoped=1; done
wants() { [ "$scoped" = 0 ] || [ -n "${!1:-}" ]; }

failed=()
section() {
  local name=$1; shift
  echo "== $name"
  if "$@"; then echo "== $name: ok"; else echo "== $name: FAILED"; failed+=("$name"); fi
}
need() {
  command -v "$1" >/dev/null 2>&1 && return 0
  echo "$1 not found: install the versions pinned in ci/versions.env"
  return 1
}
pending() { echo "pending: $1 (see docs/00-design.md, Epics and seed tickets)"; }

lint_python() { bash scripts/lint-python.sh; }
check_commits() {
  local range=${COMMIT_RANGE:-origin/master..HEAD} ok=0
  if [ -n "${PR_TITLE:-}" ]; then python3 scripts/commit_check.py --title "$PR_TITLE" || ok=1; fi
  if git rev-parse --verify -q "${range%%..*}" >/dev/null; then
    python3 scripts/commit_check.py --range "$range" || ok=1
  else
    echo "no ${range%%..*} to compare with; skipping the commit range"
  fi
  return $ok
}
check_prose() {
  local base=${COMMIT_RANGE:-origin/master..HEAD}
  base=${base%%..*}
  if git rev-parse --verify -q "$base" >/dev/null; then
    python3 scripts/prose_check.py --verify --base "$base"
  else
    echo "no $base to compare with; skipping the prose gate"
  fi
}
spec_coverage() {
  local base=${COMMIT_RANGE:-origin/master..HEAD}
  base=${base%%..*}
  if git rev-parse --verify -q "$base" >/dev/null; then
    python3 tools/spec_coverage.py --base "$(git merge-base "$base" HEAD)"
  else
    python3 tools/spec_coverage.py
  fi
}
lint_workflows() { need actionlint && actionlint .github/workflows/*.yml; }

if wants LINT; then
  section layout python3 scripts/check_layout.py
  section python-lint lint_python
  section c-format bash scripts/c-format.sh
  section cppcheck bash scripts/cppcheck.sh
  section workflow-lint lint_workflows
  section vectors python3 tools/vectors_check.py
  section spec-coverage spec_coverage
  section zone python3 tools/zone_check.py
fi
if wants COMMITS; then
  section commits check_commits
  section prose check_prose
fi
if wants SCRIPTS; then
  section scripts-tests python3 -m pytest scripts tools
fi
if wants CORE; then section core bash scripts/core-test.sh; fi
if wants GEN; then section gen python3 -m pytest gen; fi
if wants BRIDGE; then section bridge pending "H1"; fi
if wants E2E; then section e2e pending "V1, V3"; fi

if [ "${#failed[@]}" -gt 0 ]; then
  echo "failed sections: ${failed[*]}"
  exit 1
fi
echo "all sections passed"
