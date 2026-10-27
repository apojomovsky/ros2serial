#!/usr/bin/env bash
# gcovr filter sets for scripts/coverage-gate.sh, one array per component.
# The gate judges both sides with the base commit's copy of this file, so a
# PR that narrows these still measures its head with the old set and a new
# uncovered file lowers the head figure instead of vanishing.
CORE_FILTER=(--filter 'core/src/')
BRIDGE_FILTER=(
  --filter 'bridge/r2s_bridge/(src|include)/'
  --exclude '.*/src/bridge\.cpp'
  --exclude '.*/src/main\.cpp'
  --exclude '.*/include/r2s_bridge/bridge\.hpp'
  --exclude '.*/CMakeFiles/.*'
)
