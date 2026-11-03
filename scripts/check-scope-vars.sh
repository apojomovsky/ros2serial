#!/usr/bin/env bash
# Fail a make run whose command-line variables name no scope: a misspelled
# scope (make test CROE=1) must name the valid scopes instead of silently
# running nothing or everything. Prints the error message; prints nothing
# when every name is known. R2S_* knobs pass through, and so do lowercase
# names, except a lowercase name that uppercases to a scope, which suggests
# the uppercase spelling.
set -uo pipefail

# The scopes ci-test.sh gates on, plus the shard selector it consumes, plus
# the target parameters and suite knobs make passes through.
scopes="LINT SCRIPTS SCRIPTS_SHARD COMMITS CORE GEN BRIDGE CPP E2E COVERAGE DOCS EXAMPLES ESP32 BUDGETS FUZZ README NIGHTLY SOAK"
params="TEST TARGET CMD EXAMPLE BOARD ARGS IMAGE BUILD_ARGS BASE PR_BODY PR_TITLE COMMIT_RANGE"
bad=()
for name in "$@"; do
  case "$name" in R2S_*) continue ;; esac
  if [[ " $scopes $params " == *" $name "* ]]; then continue; fi
  upper=$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')
  if [[ " $scopes " == *" $upper "* ]]; then
    bad+=("$name (did you mean $upper?)")
  elif [[ "$name" =~ ^[A-Z][A-Z0-9_]*$ ]]; then
    bad+=("$name")
  fi
done
if [ "${#bad[@]}" -gt 0 ]; then
  echo "unknown make variable(s): ${bad[*]}. Valid scopes: $scopes. See make help."
fi
