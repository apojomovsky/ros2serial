#!/usr/bin/env bash
# Run the spike firmware in simavr and drive it with the host script.
# Usage: run_sim_test.sh <k1_spike.elf> [seconds] [reset_after_seconds]
# Needs simavr's development package (libsimavr-dev) and pyserial.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
elf=$1; secs=${2:-15}; reset_at=${3:-}
work=$(mktemp -d)
gcc -O2 -Wall -I/usr/include/simavr -o "$work/uno_sim" "$here/sim/uno_sim.c" -lsimavr -lsimavrparts -lelf -lpthread -lutil
"$work/uno_sim" "$elf" > "$work/sim.out" 2>&1 &
sim=$!
trap 'kill $sim 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q '/dev/pts/' "$work/sim.out" && break; sleep 0.1; done
pty=$(grep -o '/dev/pts/[0-9]*' "$work/sim.out" | head -1)
if [ -n "$reset_at" ]; then (sleep "$reset_at"; kill -USR1 $sim) & fi
python3 "$here/host/spike_host.py" --port "$pty" --seconds "$secs" --boot-wait 1.5
