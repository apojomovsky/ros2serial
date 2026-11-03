#!/usr/bin/env bash
# make check: run the scopes CI would run for the current diff, plus the fast
# gates, and print what it chose. A diff the scope map sends to the full
# suite still runs only the fast gates locally, leaving the full suite to CI;
# R2S_FULL=1 runs everything. Caller scopes stay and the computed scopes add,
# with a caller E2E and a computed E2E merging to both.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
base=${BASE:-origin/master}
suite=${R2S_SUITE:-$root/scripts/ci-test.sh}

if [ "${R2S_FULL:-}" = 1 ]; then
  echo "check: R2S_FULL=1: running the full suite"
  bash "$suite"
  exit $?
fi

# CI mode on purpose: the check runs what CI would run for this diff,
# not the cheap scopes a local run settles for.
computed=$(GITHUB_ACTIONS=1 python3 "$root/scripts/scopes_for_diff.py" --base "$base" 2>/dev/null) || computed="FULL=1"

# A caller E2E and a computed E2E merge to both, so neither backend is dropped.
computed_e2e=""
computed_rest=""
for s in $computed; do
  case $s in E2E=*) computed_e2e=$s ;; *) computed_rest="$computed_rest $s" ;; esac
done
if [ -n "$computed_e2e" ]; then
  if [ -z "${E2E:-}" ] || [ "E2E=$E2E" = "$computed_e2e" ]; then
    computed_rest="$computed_rest $computed_e2e"
  else
    computed_rest="$computed_rest E2E=both"
  fi
fi

# The caller's own scopes stay, so check also runs what it was asked to run.
# shellcheck disable=SC2086 # each scope is one assignment per word.
caller=""
for v in LINT SCRIPTS COMMITS CORE GEN BRIDGE CPP COVERAGE DOCS EXAMPLES BUDGETS FUZZ README NIGHTLY SOAK; do
  [ -n "${!v:-}" ] && caller="$caller $v=1"
done
[ -n "${E2E:-}" ] && [ -z "$computed_e2e" ] && caller="$caller E2E=$E2E"
[ -n "${SCRIPTS_SHARD:-}" ] && caller="$caller SCRIPTS_SHARD=$SCRIPTS_SHARD"
scopes="$caller$computed_rest"

echo "check: scopes for the diff against $base:$scopes"
# The mermaid check runs on the host through docker run (make lint-docs),
# never inside the dev image, so the suite cannot run it from here.
case "$scopes" in *" MERMAID=1"*) echo "check: mermaid blocks selected; run make lint-docs on the host (needs docker)" ;; esac
if [ "$scopes" = " FULL=1" ]; then
  echo "check: the diff maps to FULL; running the fast gates locally, leaving the full suite to CI"
  env LINT=1 COMMITS=1 bash "$suite"
else
  # shellcheck disable=SC2086 # each scope is one assignment per word.
  env $scopes bash "$suite"
fi
