#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# One shared apt entry point for the image builds and the CI jobs, so a
# transient mirror failure costs seconds instead of a whole rerun. Usage:
#   apt-install.sh update
#   apt-install.sh install -y --no-install-recommends <packages...>
# Bound: 5 attempts, waiting 20, 40, 60, then 80 seconds between them:
# about three minutes of backoff in total, plus apt's own
# Acquire::Retries=3 inside each attempt. Runner logs show a two minute
# mirror outage clearing on a later attempt, while a fifth attempt has never
# turned a genuine failure (a removed package, a wrong name) into a success;
# it only spends a few apt runs and minutes before the real error still fails
# the job. R2S_APT_RETRY_DELAY overrides the base backoff step in seconds;
# the tests set it to 0.
set -uo pipefail

usage() { echo "usage: apt-install.sh update | install [APT-ARGS...]" >&2; }
[ $# -ge 1 ] || { usage; exit 2; }

tries=5
delay="${R2S_APT_RETRY_DELAY:-20}"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# apt's own retries cover a fetch that recovers mid-command. These patterns
# are the ones where apt gives up and only a fresh command helps: a stale
# index with no candidate, a fetch that never completed, an HTTP 5xx.
retryable='Failed to fetch|has no installation candidate|[Hh][Tt][Tt][Pp][^ ]* 5[0-9][0-9]|Service Unavailable|Bad Gateway|Gateway Timeout|Internal Server Error'

attempt=0
rc=1
while [ "$attempt" -lt "$tries" ]; do
    attempt=$((attempt + 1))
    if apt-get -o Acquire::Retries=3 "$@" 2>&1 | tee "$tmp"; then
        exit 0
    fi
    rc=${PIPESTATUS[0]}
    if [ "$attempt" -ge "$tries" ]; then
        echo "apt-install.sh: package index still unreachable after $tries attempts (rc=$rc); rerun the job" >&2
        exit "$rc"
    fi
    if ! grep -Eiq "$retryable" "$tmp"; then
        echo "apt-install.sh: attempt $attempt/$tries failed (rc=$rc), not retryable, giving up" >&2
        exit "$rc"
    fi
    pause=$((attempt * delay))
    echo "apt-install.sh: attempt $attempt/$tries failed (rc=$rc), retrying in ${pause}s" >&2
    sleep "$pause"
done
