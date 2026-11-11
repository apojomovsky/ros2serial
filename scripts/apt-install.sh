#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# One shared apt entry point for the image builds and the CI jobs, so a
# transient mirror failure costs seconds instead of a whole rerun. Usage:
#   apt-install.sh update
#   apt-install.sh install -y --no-install-recommends <packages...>
# Bound: 5 attempts, waiting 20, 40, 60, then 80 seconds between them:
# about three minutes of backoff in total, plus apt's own
# Acquire::Retries=3 inside each attempt. The retries ride out short
# failures; Acquire::ForceIPv4=true removes the address-family variable.
# Neither helps when the runner cannot resolve the mirror names at all:
# then the helper swaps the Ubuntu entries to azure.archive.ubuntu.com,
# the mirror built for the runners' network, and retries against it.
# R2S_APT_RETRY_DELAY overrides the base backoff step in seconds, and
# R2S_APT_SOURCES_DIR overrides the apt sources root the swap rewrites;
# the tests set the delay to 0 and point the root at fixtures.
set -uo pipefail

usage() { echo "usage: apt-install.sh update | install [APT-ARGS...]" >&2; }
[ $# -ge 1 ] || { usage; exit 2; }

tries=5
delay="${R2S_APT_RETRY_DELAY:-20}"
sources_dir="${R2S_APT_SOURCES_DIR:-/etc/apt}"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# Point the Ubuntu entries at the Azure mirror after a name-resolution
# failure: the GitHub runners live on Azure and the generic mirror names
# stop resolving there, while the trafficmanager name keeps working.
# Only files mentioning the Ubuntu mirrors are rewritten; anything else
# (rosdep sources, third-party entries) is left untouched.
swap_ubuntu_mirror() {
    local file changed=""
    for file in "$sources_dir/sources.list" "$sources_dir"/sources.list.d/*.sources; do
        [ -f "$file" ] || continue
        if grep -Eq 'archive\.ubuntu\.com|security\.ubuntu\.com' "$file"; then
            sed -i -e 's/archive\.ubuntu\.com/azure.archive.ubuntu.com/g' \
                -e 's/security\.ubuntu\.com/azure.archive.ubuntu.com/g' "$file"
            changed="${changed:+$changed }${file##*/}"
        fi
    done
    if [ -n "$changed" ]; then
        echo "apt-install.sh: Could not resolve the Ubuntu mirrors; switching $changed to azure.archive.ubuntu.com for the rest of the run" >&2
    else
        echo "apt-install.sh: Could not resolve the Ubuntu mirrors; no Ubuntu entries under $sources_dir" >&2
    fi
}

# apt's own retries cover a fetch that recovers mid-command. These patterns
# are the ones where apt gives up and only a fresh command helps: a stale
# index with no candidate, a fetch that never completed, an HTTP 5xx.
retryable='Failed to fetch|has no installation candidate|[Hh][Tt][Tt][Pp][^ ]* 5[0-9][0-9]|Service Unavailable|Bad Gateway|Gateway Timeout|Internal Server Error'

attempt=0
rc=1
swapped=0
while [ "$attempt" -lt "$tries" ]; do
    attempt=$((attempt + 1))
    if apt-get -o Acquire::Retries=3 -o Acquire::ForceIPv4=true "$@" 2>&1 | tee "$tmp"; then
        exit 0
    fi
    rc=${PIPESTATUS[0]}
    if grep -Eiq 'Could not resolve' "$tmp" && [ "$swapped" -eq 0 ] && [ "$attempt" -lt "$tries" ]; then
        swap_ubuntu_mirror
        swapped=1
    fi
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
