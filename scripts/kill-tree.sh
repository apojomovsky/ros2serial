#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# TERM a process tree, then KILL what lingers, so a killed launch wrapper
# cannot orphan its node into the next leg. Only the pid and its own
# descendants are ever signalled, never anything else on the host.
# Usage: kill-tree.sh <pid> [wait-secs, default 10]
set -uo pipefail

[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: kill-tree.sh <pid> [wait-secs]" >&2; exit 2; }
root=$1
wait_secs=${2:-10}
case "$root" in '' | *[!0-9]*) echo "usage: kill-tree.sh <pid> [wait-secs]" >&2; exit 2 ;; esac
case "$wait_secs" in '' | *[!0-9]* | 0) echo "usage: kill-tree.sh <pid> [wait-secs]" >&2; exit 2 ;; esac
if [ "$root" = "1" ] || [ "$root" = "$$" ]; then
    echo "kill-tree.sh: refusing to kill $root" >&2
    exit 2
fi

children_of() {
    # Direct children of one pid, read from /proc so no tool is needed.
    local want=$1 stat contents rest pid ppid
    for stat in /proc/[0-9]*/stat; do
        [ -r "$stat" ] || continue
        contents=$(cat "$stat" 2>/dev/null) || continue
        rest=${contents##*)}
        pid=${stat#/proc/}
        pid=${pid%%/*}
        set -- $rest
        ppid=$2
        [ "$ppid" = "$want" ] && echo "$pid"
    done
}

tree_pids() {
    # The root plus every descendant, breadth-first, without duplicates.
    local queue=$1 pid kid seen=" $1 " out="$1"
    while [ -n "$queue" ]; do
        pid=${queue%% *}
        if [ "$queue" = "$pid" ]; then queue=""; else queue=${queue#* }; fi
        for kid in $(children_of "$pid"); do
            case "$seen" in *" $kid "*) ;; *) seen="$seen$kid "; out="$out $kid"; queue="$queue $kid" ;; esac
        done
    done
    # The trailing space the last append left would signal an empty pid.
    echo "${out% }"
}

alive() {
    # A missing /proc entry or a zombie counts as dead: neither holds a port.
    local contents rest
    contents=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
    rest=${contents##*)}
    set -- $rest
    [ "$1" = "Z" ] && return 1
    return 0
}

# shellcheck disable=SC2086 # tree_pids prints space-separated pids on purpose.
tree=$(tree_pids "$root")
kill $tree 2>/dev/null || true
deadline=$((SECONDS + wait_secs))
while :; do
    # shellcheck disable=SC2086 # same list as above.
    left=""
    for pid in $tree; do alive "$pid" && left="$left $pid"; done
    [ -z "$left" ] && exit 0
    [ "$SECONDS" -ge "$deadline" ] && break
    sleep 0.2
done
# shellcheck disable=SC2086 # the survivors just collected.
kill -9 $left 2>/dev/null || true
sleep 1
# shellcheck disable=SC2086 # same list as above.
left=""
for pid in $tree; do alive "$pid" && left="$left $pid"; done
[ -z "$left" ] && exit 0
echo "kill-tree.sh: survivors:$left" >&2
exit 1
