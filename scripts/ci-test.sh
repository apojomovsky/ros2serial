#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The single suite script: CI runs exactly this, and so does `make test`.
# Scope with one or more of LINT=1 SCRIPTS=1 COMMITS=1 CORE=1 GEN=1 BRIDGE=1
# CPP=1 E2E=<backend> COVERAGE=1 DOCS=1 EXAMPLES=1 ESP32=1 PIC=1 PLATFORMIO=1 STM32=1 RP2040=1 BUDGETS=1
# FUZZ=1 README=1 NIGHTLY=1 SOAK=1; no scope runs everything.
# E2E takes posix, avr, or both; both runs each backend once in one scope.
# SCRIPTS_SHARD=N runs one CI shard of the SCRIPTS scope; unset runs it whole.
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

# SCRIPTS_SHARD is consumed here too: the section reads the copy below, and
# the inherited variable is unset so nested suite runs the script tests start
# select only what their own caller sets.
scripts_shard=${SCRIPTS_SHARD:-}
unset SCRIPTS_SHARD

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
for v in LINT SCRIPTS COMMITS CORE GEN BRIDGE AMENT_LINT CPP E2E COVERAGE DOCS EXAMPLES ESP32 PIC PLATFORMIO STM32 RP2040 BUDGETS FUZZ README NIGHTLY SOAK; do [ -n "${!v:-}" ] && scoped=1; done
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
  section headers python3 scripts/check_headers.py
  section c-format bash scripts/c-format.sh
  section cppcheck bash scripts/cppcheck.sh
  section workflow-lint lint_workflows
  section vectors python3 tools/vectors_check.py
  section spec-coverage spec_coverage
  section zone python3 tools/zone_check.py
  section taskfile-coverage python3 tools/taskfile_coverage.py
  section rosdep-audit python3 tools/rosdep_audit.py
fi
if wants COMMITS; then scope_now=COMMITS
  section commits check_commits
  section prose check_prose
fi
scripts_tests() {
  # scripts_shard selects one CI shard (0 to N-1) of the collected node ids;
  # empty runs the whole scope, exactly as before. A stuck test dumps every
  # thread's stack past the timeout, and the slowest tests are always listed,
  # so the next hang names its test instead of going silent.
  if [ -n "$scripts_shard" ]; then
    python3 -m pytest scripts tools --shard="$scripts_shard" -o faulthandler_timeout=300 --durations=10
  else
    python3 -m pytest scripts tools -o faulthandler_timeout=300 --durations=10
  fi
}
if wants SCRIPTS; then scope_now=SCRIPTS
  section scripts-tests scripts_tests
fi
if wants CORE; then scope_now=CORE; section core bash scripts/core-test.sh; fi
gen_tests() {
  local host_dir="${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/host"
  bash scripts/build-host-lib.sh "$host_dir" && R2S_HOST_LIB_DIR="$host_dir" python3 -m pytest gen
}
if wants GEN; then scope_now=GEN; section gen gen_tests; fi
if wants BRIDGE; then scope_now=BRIDGE; section bridge bash scripts/bridge-test.sh; fi
if wants AMENT_LINT; then scope_now=AMENT_LINT; section ament-lint bash scripts/ament-lint.sh; fi
if wants CPP; then scope_now=CPP; section cpp bash scripts/cpp-test.sh; fi
if wants COVERAGE; then scope_now=COVERAGE; section coverage bash scripts/coverage-gate.sh; fi
if wants DOCS; then scope_now=DOCS; section docs python3 tools/snippets_check.py; section site python3 tools/site_build.py; section site-links python3 tools/site_links.py; fi
if wants EXAMPLES; then scope_now=EXAMPLES; section examples bash scripts/build-examples.sh; fi
if wants ESP32; then scope_now=ESP32; section esp32 bash scripts/build-examples-esp32.sh; fi
if wants PIC; then scope_now=PIC; section pic bash scripts/build-pic-canary.sh; fi
if wants PLATFORMIO; then scope_now=PLATFORMIO; section platformio bash scripts/build-examples-pio.sh; fi
if wants STM32; then scope_now=STM32; section stm32 bash ports/stm32/build.sh; fi
if wants RP2040; then scope_now=RP2040; section rp2040 bash ports/rp2040/build.sh; fi
if wants BUDGETS; then scope_now=BUDGETS; section budgets bash scripts/budgets-check.sh; fi
if wants FUZZ; then scope_now=FUZZ; section fuzz bash scripts/fuzz.sh; fi
if wants README; then scope_now=README; section readme bash scripts/check-readme.sh; fi
if wants NIGHTLY; then scope_now=NIGHTLY; section e2e-isolation python3 -m pytest -s tests/e2e/test_isolation.py; fi
if wants SOAK; then scope_now=SOAK; section soak bash tests/e2e/run-soak.sh; fi
if wants E2E; then scope_now=E2E
  case "${E2E:-}" in
    "") section e2e-avr-harness bash tests/harness/simavr/run.sh
        section e2e-avr bash tests/e2e/run-avr.sh
        section e2e-posix bash tests/e2e/run.sh ;;
    avr) section e2e-avr-harness bash tests/harness/simavr/run.sh
        section e2e-avr bash tests/e2e/run-avr.sh ;;
    posix) section e2e-posix bash tests/e2e/run.sh ;;
    both) section e2e-avr-harness bash tests/harness/simavr/run.sh
        section e2e-avr bash tests/e2e/run-avr.sh
        section e2e-posix bash tests/e2e/run.sh ;;
    *)
      echo "unknown E2E backend '${E2E}': known are posix, avr and both" >&2
      failed+=("e2e")
      ;;
  esac
fi

empty=""
if [ "$scoped" = 1 ]; then
  for v in LINT SCRIPTS COMMITS CORE GEN BRIDGE AMENT_LINT CPP E2E COVERAGE DOCS EXAMPLES ESP32 PIC PLATFORMIO STM32 RP2040 BUDGETS FUZZ README NIGHTLY SOAK; do
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
