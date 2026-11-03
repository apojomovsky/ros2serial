#!/usr/bin/env bash
# Run the stale suite tasks: map the ci-test.sh scope variables to Taskfile
# task names, then let `task` skip every task whose inputs match its last
# successful run. Naming no scope runs the default task, which holds every
# section but the host-only mermaid check. R2S_FULL=1 forces all of them.
# R2S_SUITE points the runner at another command (tests point it at a
# recorder), which receives the task names as arguments; the scope
# variables stay exported, so the command sees what `task` would.
set -uo pipefail

names=()
for scope in LINT COMMITS SCRIPTS CORE GEN BRIDGE CPP COVERAGE DOCS EXAMPLES BUDGETS FUZZ README NIGHTLY; do
  [ -n "${!scope:-}" ] && names+=("$(printf '%s' "$scope" | tr '[:upper:]' '[:lower:]')")
done
case "${E2E:-}" in
  "") ;;
  posix) names+=(e2e-posix) ;;
  avr) names+=(e2e-avr) ;;
  both) names+=(e2e-avr e2e-posix) ;;
  *) echo "run-tasks: unknown E2E backend '${E2E}': known are posix, avr and both" >&2; exit 2 ;;
esac

args=()
[ "${R2S_FULL:-}" = 1 ] && args+=(--force)
args+=("${names[@]}")
if [ -n "${R2S_SUITE:-}" ]; then
  # A script path, run as the ritual always ran it: no executable bit needed.
  # shellcheck disable=SC2068 # each task name is one argument.
  bash "${R2S_SUITE}" ${args[@]+"${args[@]}"}
  exit $?
fi
command -v task >/dev/null 2>&1 || {
  echo "task not found: install the version pinned in ci/versions.env"
  exit 1
}
# One task at a time: sections share the suite lock and the build tree.
# shellcheck disable=SC2068 # each task name is one argument.
exec task --concurrency 1 ${args[@]+"${args[@]}"}
