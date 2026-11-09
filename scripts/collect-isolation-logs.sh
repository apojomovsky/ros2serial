#!/usr/bin/env bash
# Gather what the e2e isolation pairs leave behind for the artifact upload.
# Per-scenario logs live under each section's build dir and full section
# outputs under e2e-section-logs; a section that never built leaves
# nothing to copy. Missing inputs are fine, so this never fails the job.
# Usage: collect-isolation-logs.sh <cache-dir> <dest-dir>
set -uo pipefail
[ $# -eq 2 ] || { echo "usage: collect-isolation-logs.sh <cache-dir> <dest-dir>" >&2; exit 2; }
cache=$1
dest=$2
mkdir -p "$dest" || exit 1
if [ -d "$cache/e2e-section-logs" ]; then
  cp "$cache"/e2e-section-logs/*.log "$dest"/ 2>/dev/null || true
fi
for logs in "$cache"/build-*/e2e/*/scenario-logs; do
  [ -d "$logs" ] || continue
  # Namespaced by build dir, so the two pair sections keep both.
  build=$(basename "$(dirname "$(dirname "$(dirname "$logs")")")")
  for log in "$logs"/*.log; do
    [ -f "$log" ] || continue
    mkdir -p "$dest/$build" || exit 1
    cp "$log" "$dest/$build"/ || exit 1
  done
done
exit 0
