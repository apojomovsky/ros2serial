#!/usr/bin/env bash
# make check: run the stale tasks for the current tree and print what was
# selected. Caller scopes stay: they select the tasks to consider, and the
# checksums decide which of those run. R2S_FULL=1 forces all of them.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

caller=""
for v in LINT SCRIPTS COMMITS CORE GEN BRIDGE CPP COVERAGE DOCS EXAMPLES BUDGETS FUZZ README NIGHTLY SOAK; do
  [ -n "${!v:-}" ] && caller="$caller $v=1"
done
[ -n "${E2E:-}" ] && caller="$caller E2E=$E2E"
if [ "${R2S_FULL:-}" = 1 ]; then
  echo "check: R2S_FULL=1: running the full suite"
else
  echo "check: running the stale tasks for the current tree:$caller"
fi
# The mermaid check is host-only (make lint-docs), so it stays out of the
# tasks here; CI gates it on its own status.
bash "$root/scripts/run-tasks.sh"
