#!/usr/bin/env bash
# Run a command until it passes, bounded: a per-attempt timeout and a capped
# try count, with the last attempt's output kept so a real regression stays
# visible. Usage: with-retry.sh --tries N --timeout SECS --out FILE -- CMD...
set -uo pipefail

usage() { echo "usage: with-retry.sh --tries N --timeout SECS --out FILE -- COMMAND..." >&2; }

tries=""
timeout_secs=""
out=""
while [ $# -gt 0 ]; do
    case "$1" in
    --tries)
        [ $# -ge 2 ] || { usage; exit 2; }
        tries=$2
        shift 2
        ;;
    --timeout)
        [ $# -ge 2 ] || { usage; exit 2; }
        timeout_secs=$2
        shift 2
        ;;
    --out)
        [ $# -ge 2 ] || { usage; exit 2; }
        out=$2
        shift 2
        ;;
    --) shift; break ;;
    *) usage; exit 2 ;;
    esac
done
[ $# -ge 1 ] || { usage; exit 2; }
case "$tries" in '' | *[!0-9]* | 0) usage; exit 2 ;; esac
case "$timeout_secs" in '' | *[!0-9]* | 0) usage; exit 2 ;; esac
[ -n "$out" ] || { usage; exit 2; }

attempt=0
rc=1
while [ "$attempt" -lt "$tries" ]; do
    attempt=$((attempt + 1))
    if timeout "$timeout_secs" "$@" >"$out" 2>&1; then
        exit 0
    else
        rc=$?
        if [ "$attempt" -lt "$tries" ]; then
            echo "with-retry.sh: attempt $attempt/$tries failed (rc=$rc), retrying" >&2
        fi
    fi
done
echo "with-retry.sh: $tries attempts failed, last rc=$rc, last output:" >&2
cat "$out" >&2
exit "$rc"
