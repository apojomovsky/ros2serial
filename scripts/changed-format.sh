#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# changed-format.sh: pre-push format gate on the lines a push adds. Reads
# git pre-push lines on stdin, diffs each updated ref, and runs ament_cpplint
# on C/C++ plus ruff check and ruff format on Python, failing on a finding.
# Only added lines are judged: most of the tree carries cpplint findings CI
# never gates (header guards, include order), so file scope would block
# pushes CI accepts. Width owner is cpplint at 100 columns, the same 100 as
# uncrustify code_width; uncrustify judges whole-format and runs only with
# its pinned binary. A missing tool prints one SKIP line and never blocks.
set -uo pipefail
zero=0000000000000000000000000000000000000000
base=${BASE:-origin/master}
top=$(git rev-parse --show-toplevel 2>/dev/null || true)
[ -n "$top" ] || { echo 'changed-format: not a git tree, leaving it to CI'; exit 0; }

# Run from the tree root, so ruff finds the root config and reports paths
# relative to it no matter which subdirectory the push ran from.
cd "$top" || { echo 'changed-format: cannot enter the tree, leaving it to CI'; exit 0; }

# Added lines per pushed ref, as "absolute-path<TAB>lineno" records.
added=$(while read -r local_ref local_oid remote_ref remote_oid || [ -n "${local_ref:-}" ]; do
  [ -n "${local_ref:-}" ] || continue
  [ "$local_oid" = "$zero" ] && continue
  if [ "$remote_oid" = "$zero" ]; then
    old=$(git merge-base "$base" "$local_oid" 2>/dev/null || true)
    [ -n "$old" ] || {
      echo "changed-format: no $base for $local_ref, leaving it to CI" >&2
      continue
    }
  else
    old=$remote_oid
  fi
  git -C "$top" diff -U0 --no-color --no-ext-diff --diff-filter=ACMR \
    "$old" "$local_oid" -- '*.c' '*.h' '*.cpp' '*.hpp' '*.cc' '*.py' 2>/dev/null \
  | awk -v top="$top" '
    /^\+\+\+ b\// { file = top "/" substr($0, 7); next }
    /^@@ / {
      if (match($0, /\+[0-9]+(,[0-9]+)?/)) {
        range = substr($0, RSTART + 1, RLENGTH - 1)
        n = split(range, parts, ",")
        start = parts[1] + 0
        count = (n > 1) ? parts[2] + 0 : 1
        if (file !~ /(^|\/)vendor\//) {
          for (i = 0; i < count; i++) print file "\t" (start + i)
        }
      }
      next
    }'
done)
[ -n "$added" ] || { echo 'changed-format: no C or Python lines to check'; exit 0; }

declare -A touched=()
while IFS=$'\t' read -r path lineno; do
  [ -n "${path:-}" ] && [ -n "${lineno:-}" ] || continue
  touched["$path:$lineno"]=1
done <<<"$added"

cpp=()
py=()
while IFS= read -r path; do
  case $path in
    *.c | *.h | *.cpp | *.hpp | *.cc) cpp+=("$path") ;;
    *.py) py+=("$path") ;;
  esac
done < <(printf '%s\n' "${!touched[@]}" | sed 's/:[0-9][0-9]*$//' | awk '!seen[$0]++')

failed=0

# One tool finding per output line, kept only on a pushed line.
scoped() {
  local path='' line='' rest='' kept=1
  while IFS=$'\t' read -r path line rest; do
    if [ -n "${touched["$path:$line"]:-}" ]; then
      printf '%s\n' "$rest"
      kept=0
    fi
  done
  return $kept
}

split_finding() {
  awk -v top="$top" '{ if (match($0, /^[^:]+:[0-9]+:/)) {
    head = substr($0, 1, RLENGTH - 1)
    n = split(head, parts, ":")
    path = parts[1]
    if (path !~ /^\//) path = top "/" path
    print path "\t" parts[n] "\t" $0
  } }'
}

if [ "${#cpp[@]}" -gt 0 ]; then
  if command -v ament_cpplint >/dev/null 2>&1; then
    lint=('ament_cpplint')
  elif command -v cpplint >/dev/null 2>&1; then
    lint=('cpplint' '--linelength=100')
  else
    echo 'changed-format: SKIP C++ checks, no ament_cpplint or cpplint (left to CI)'
    lint=()
  fi
  if [ "${#lint[@]}" -gt 0 ]; then
    out=$("${lint[@]}" "${cpp[@]}" 2>&1 || true)
    kept=$(printf '%s\n' "$out" | split_finding | scoped || true)
    if [ -n "$kept" ]; then
      printf '%s\n' "$kept"
      failed=1
    fi
  fi
fi

if [ "${#py[@]}" -gt 0 ]; then
  pin=$(grep -m1 '^RUFF_VERSION=' "$top/ci/versions.env" 2>/dev/null || true)
  pin=${pin#RUFF_VERSION=}
  ver=$(python3 -m ruff --version 2>/dev/null || echo '')
  if [ -z "$pin" ] || [ "$ver" != "ruff $pin" ]; then
    echo 'changed-format: SKIP Python checks, no pinned ruff (left to CI)'
  else
    out=$(python3 -m ruff check --output-format=concise "${py[@]}" 2>&1 || true)
    kept=$(printf '%s\n' "$out" | split_finding | scoped || true)
    if [ -n "$kept" ]; then
      printf '%s\n' "$kept"
      failed=1
    fi
    if ! fmt_out=$(python3 -m ruff format --check "${py[@]}" 2>&1); then
      printf '%s\n' "$fmt_out"
      failed=1
    fi
  fi
fi

if [ "${#cpp[@]}" -gt 0 ]; then
  here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
  if command -v uncrustify >/dev/null 2>&1 \
    && uncrustify --version 2>/dev/null | grep -q '0\.78\.1' \
    && [ -f "$here/c-format.sh" ]; then
    if ! fmt_out=$(bash "$here/c-format.sh" "${cpp[@]}" 2>&1); then
      printf '%s\n' "$fmt_out"
      failed=1
    fi
  else
    echo 'changed-format: SKIP uncrustify check, no pinned uncrustify (left to CI)'
  fi
fi

exit $failed
