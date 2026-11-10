#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Run the stale suite tasks: map the ci-test.sh scope variables to Taskfile
# task names, then let `task` skip every task whose inputs match its last
# successful run. Naming no scope runs the default task, which holds every
# section but the host-only mermaid check; with R2S_LOCAL=1 it runs the
# local task instead, the same set without the CI-only sections: the
# end-to-end ones, esp32 and pic, whose toolchains install only in their
# CI jobs (the pre-push hook and the takeoff set it). A named scope, E2E
# included, still runs. R2S_FULL=1 forces all of them.
# R2S_SUITE points the runner at another command (tests point it at a
# recorder), which receives the task names as arguments; the scope
# variables stay exported, so the command sees what `task` would.
set -uo pipefail

names=()
for scope in LINT COMMITS SCRIPTS CORE GEN BRIDGE AMENT_LINT CPP COVERAGE DOCS EXAMPLES ESP32 PIC BUDGETS FUZZ README NIGHTLY SOAK; do
  [ -n "${!scope:-}" ] && names+=("$(printf '%s' "$scope" | tr '[:upper:]' '[:lower:]')")
done
case "${E2E:-}" in
  "") ;;
  posix) names+=(e2e-posix) ;;
  avr) names+=(e2e-avr) ;;
  both) names+=(e2e-avr e2e-posix) ;;
  *) echo "run-tasks: unknown E2E backend '${E2E}': known are posix, avr and both" >&2; exit 2 ;;
esac

if [ ${#names[@]} -eq 0 ] && [ "${R2S_LOCAL:-}" = 1 ]; then
  names=(local)
fi

args=()
[ "${R2S_FULL:-}" = 1 ] && args+=(--force)
args+=("${names[@]}")
# Name the attempt up front: a bare task exit code says nothing about
# which sections were even tried.
forced=""
[ "${R2S_FULL:-}" = 1 ] && forced=" --force"
if [ ${#names[@]} -eq 0 ]; then
  attempt="the default task$forced"
else
  attempt="tasks [${names[*]}]$forced"
fi
echo "run-tasks: running $attempt"
if [ -n "${R2S_SUITE:-}" ]; then
  # A script path, run as the ritual always ran it: no executable bit needed.
  # shellcheck disable=SC2068 # each task name is one argument.
  bash "${R2S_SUITE}" ${args[@]+"${args[@]}"}
  rc=$?
else
  command -v task >/dev/null 2>&1 || {
    echo "task not found: install the version pinned in ci/versions.env"
    exit 1
  }
  # One task at a time: sections share the suite lock and the build tree.
  # shellcheck disable=SC2068 # each task name is one argument.
  task --concurrency 1 ${args[@]+"${args[@]}"}
  rc=$?
fi
# Name the failed attempt: task's own line carries the section, this one
# carries the attempt and the exit code, so a truncated log still acts.
[ $rc -ne 0 ] && echo "run-tasks: FAILED $attempt (exit $rc)" >&2
exit $rc
