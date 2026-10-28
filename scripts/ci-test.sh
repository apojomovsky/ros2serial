#!/usr/bin/env bash
# The single suite script: CI runs exactly this, and so does `make test`.
# Scope with one or more of LINT=1 SCRIPTS=1 COMMITS=1 CORE=1 GEN=1 BRIDGE=1
# E2E=<backend> COVERAGE=1 DOCS=1 EXAMPLES=1 FUZZ=1; no scope runs everything.
# COMMITS checks COMMIT_RANGE (default origin/master..HEAD) and PR_TITLE when set,
# then runs the prose gate on the same range. Exits non-zero if any section fails.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/clean-git-env.sh
source "$root/scripts/clean-git-env.sh"
cd "$root" || exit 2

# TEST goes here and not further down: the script tests call the ritual, and a
# ritual that sees TEST=1 runs the suite again, so the pair would fork.
unset TEST

# One suite at a time per tree. A second run would wait on the same lock and
# then test a tree the first run had already changed, so it says so and stops
# instead. scripts/suite-lock.sh keys the lock by the tree, so a copy takes its
# own lock and does not collide with the run that copied it, nor with a copy
# at the same path in another container.
suite_lock=$(bash "$root/scripts/suite-lock.sh" "$root") || {
  echo "the suite lock cannot be located for this tree" >&2
  exit 1
}
if [ -n "${R2S_SUITE_LOCK:-}" ] && [ "${R2S_SUITE_LOCK_TREE:-}" = "$root" ]; then
  suite_lock=$R2S_SUITE_LOCK
fi
mkdir -p "$(dirname "$suite_lock")" 2>/dev/null || {
  echo "the suite lock directory cannot be created ($suite_lock)" >&2
  echo "set R2S_CACHE_DIR or TMPDIR to a writable directory" >&2
  exit 1
}
# The lock is taken here and held for the whole run, so a second suite run
# for the tree is refused. The script does not re-exec itself to take it: the
# suite runs this script, so a self re-exec would nest without end. A host
# without flock runs unlocked, as the build lock does.
if command -v flock >/dev/null 2>&1; then
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
    echo "the suite is already running for this repository ($root, lock $suite_lock)" >&2
    echo "wait for it, or set R2S_SUITE_LOCK_WAIT=1 to queue" >&2
    exit 75
  fi
else
  echo "the suite lock is not taken: flock is not on this host" >&2
fi

scoped=0
for v in LINT SCRIPTS COMMITS CORE GEN BRIDGE E2E COVERAGE DOCS EXAMPLES FUZZ; do [ -n "${!v:-}" ] && scoped=1; done
wants() { [ "$scoped" = 0 ] || [ -n "${!1:-}" ]; }

failed=()
ran_scopes=""
scope_now=""
section() {
  local name=$1; shift
  case " $ran_scopes " in *" $scope_now "*) ;; *) ran_scopes="$ran_scopes $scope_now" ;; esac
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
  if git rev-parse --verify -q "${range%%..*}" >/dev/null; then
    python3 scripts/commit_check.py --range "$range" || ok=1
    # The title is the subject that lands, so it must carry what the
    # commits declare.
    if [ -n "${PR_TITLE:-}" ]; then
      python3 scripts/commit_check.py --title "$PR_TITLE" --covers-range "$range" || ok=1
    fi
  else
    echo "no ${range%%..*} to compare with; skipping the commit range"
    if [ -n "${PR_TITLE:-}" ]; then python3 scripts/commit_check.py --title "$PR_TITLE" || ok=1; fi
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

if wants LINT; then scope_now=LINT
  section layout python3 scripts/check_layout.py
  section python-lint lint_python
  section c-format bash scripts/c-format.sh
  section cppcheck bash scripts/cppcheck.sh
  section workflow-lint lint_workflows
  section vectors python3 tools/vectors_check.py
  section spec-coverage spec_coverage
  section zone python3 tools/zone_check.py
fi
if wants COMMITS; then scope_now=COMMITS
  section commits check_commits
  section prose check_prose
fi
if wants SCRIPTS; then scope_now=SCRIPTS
  section scripts-tests python3 -m pytest scripts tools
fi
if wants CORE; then scope_now=CORE; section core bash scripts/core-test.sh; fi
if wants GEN; then scope_now=GEN; section gen python3 -m pytest gen; fi
if wants BRIDGE; then scope_now=BRIDGE; section bridge bash scripts/bridge-test.sh; fi
if wants COVERAGE; then scope_now=COVERAGE; section coverage bash scripts/coverage-gate.sh; fi
if wants DOCS; then scope_now=DOCS; section docs python3 tools/snippets_check.py; fi
if wants EXAMPLES; then scope_now=EXAMPLES; section examples bash scripts/build-examples.sh; fi
if wants FUZZ; then scope_now=FUZZ; section fuzz bash scripts/fuzz.sh; fi
if wants E2E; then scope_now=E2E
  case "${E2E:-}" in
    "") section e2e-avr bash tests/harness/simavr/run.sh
        section e2e-posix bash tests/e2e/run.sh ;;
    avr) section e2e-avr bash tests/harness/simavr/run.sh ;;
    posix) section e2e-posix bash tests/e2e/run.sh ;;
    *)
      echo "unknown E2E backend '${E2E}': known are posix and avr" >&2
      failed+=("e2e")
      ;;
  esac
fi

empty=""
if [ "$scoped" = 1 ]; then
  for v in LINT SCRIPTS COMMITS CORE GEN BRIDGE E2E COVERAGE DOCS EXAMPLES FUZZ; do
    if [ -n "${!v:-}" ]; then
      case " $ran_scopes " in *" $v "*) ;; *) empty="$empty $v" ;; esac
    fi
  done
fi
if [ -n "$empty" ]; then
  echo "no section ran for scope:$empty" >&2
fi
if [ "${#failed[@]}" -gt 0 ]; then
  echo "failed sections: ${failed[*]}"
  exit 1
fi
if [ -n "$empty" ]; then exit 1; fi
echo "all sections passed"
