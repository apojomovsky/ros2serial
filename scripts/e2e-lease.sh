#!/usr/bin/env bash
# Lease one e2e isolation slot for the wrapped command: a DDS domain and a
# Zenoh router port no other local suite holds. The slot lock is held on an
# open file descriptor, so the lease dies with the command however it exits.
# It sits on fd 8: the build lock and the suite lock both use fd 9.
# An explicit R2S_E2E_DOMAIN_ID skips allocation; without one, slot 0 maps
# to the domain and port a lone run always used, so CI behaves as before.
# Usage: e2e-lease.sh <command> [args...]
set -uo pipefail
[ $# -ge 1 ] || { echo "usage: e2e-lease.sh <command> [args...]" >&2; exit 2; }
DOMAIN_BASE=89
PORT_BASE=7447
SLOTS=32
if [ -n "${R2S_E2E_DOMAIN_ID:-}" ]; then
  : "${R2S_ZENOH_ROUTER_PORT:=$((PORT_BASE + R2S_E2E_DOMAIN_ID - DOMAIN_BASE))}"
  export R2S_ZENOH_ROUTER_PORT
  exec "$@"
fi
if ! command -v flock >/dev/null 2>&1; then
  echo "e2e-lease: flock not found, running on the default domain" >&2
  export R2S_E2E_DOMAIN_ID=$DOMAIN_BASE
  export R2S_ZENOH_ROUTER_PORT=$PORT_BASE
  exec "$@"
fi
cache=${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}
lease_dir=$cache/e2e-leases
mkdir -p "$lease_dir" || exit 1
exec 8>"$lease_dir/alloc.lock" || exit 1
flock 8 || exit 1
slot=""
for i in $(seq 0 $((SLOTS - 1))); do
  exec 8>"$lease_dir/slot-$i.lock" || exit 1
  if flock -n 8 2>/dev/null; then
    slot=$i
    break
  fi
done
[ -n "$slot" ] || { echo "e2e-lease: no free slot in $lease_dir" >&2; exit 1; }
export R2S_E2E_DOMAIN_ID=$((DOMAIN_BASE + slot))
export R2S_ZENOH_ROUTER_PORT=$((PORT_BASE + slot))
exec "$@"
