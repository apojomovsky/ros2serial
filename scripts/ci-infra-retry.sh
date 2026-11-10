#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Rerun a failed master run once when every failed job died of infra trouble.
# The workflow guards the attempt count; this driver lists the failed jobs,
# asks scripts/ci_infra_retry.py whether each log reads as an HTTP 5xx with
# no test failure, and reruns only the failed jobs when all of them do.
set -uo pipefail

usage() { echo "usage: ci-infra-retry.sh RUN_ID" >&2; }
[ $# -eq 1 ] || { usage; exit 2; }
run_id=$1
[ -n "${GITHUB_REPOSITORY:-}" ] || {
    echo "ci-infra-retry.sh: GITHUB_REPOSITORY is required" >&2
    exit 2
}
script_dir=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ids=$(gh api "repos/${GITHUB_REPOSITORY}/actions/runs/${run_id}/jobs" --paginate --jq '.jobs[] | select(.conclusion == "failure") | .id')
[ -n "$ids" ] || {
    echo "ci-infra-retry.sh: no failed jobs in run $run_id, nothing to rerun"
    exit 0
}
logs=()
for id in $ids; do
    if ! gh run view --job "$id" --log >"$tmp/job-$id.log" 2>/dev/null; then
        echo "ci-infra-retry.sh: no log for job $id, keeping the conclusion"
        exit 0
    fi
    logs+=("$tmp/job-$id.log")
done
if python3 "$script_dir/ci_infra_retry.py" "${logs[@]}"; then
    gh run rerun "$run_id" --failed
else
    echo "ci-infra-retry.sh: not infrastructure, keeping the conclusion"
fi
