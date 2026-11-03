#!/usr/bin/env bash
# Coverage gate: fail when HEAD lowers line or branch coverage of core or
# bridge below the merge base, or below the floors in docs/01-testing.md.
# The base is measured by checking it out in a detached worktree and running
# the same measurement there, so a PR cannot relax its own judge and no
# baseline file, secret or external service is needed.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
# One isolation slot per run: a leased DDS domain, so concurrent suites never
# see each other's bridge nodes. Held for the whole run, into both the head
# and the base measurements below. The pool base is 0, the domain a lone
# coverage run always used.
if [ -z "${R2S_E2E_LEASED:-}" ]; then
  exec env R2S_E2E_LEASED=1 R2S_E2E_DOMAIN_BASE=${R2S_E2E_DOMAIN_BASE:-0} \
    bash "$root/scripts/e2e-lease.sh" bash "$root/scripts/coverage-gate.sh" "$@"
fi
# One DDS domain per suite run, so parallel runs on the shared host never see
# each other's nodes: every process below inherits the same domain.
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
echo "== coverage isolation: ROS_DOMAIN_ID=$ROS_DOMAIN_ID"

base_ref=${COMMIT_RANGE:-origin/master..HEAD}
base_ref=${base_ref%%..*}
base=""
if [ -n "$base_ref" ] && git rev-parse --verify -q "$base_ref" >/dev/null; then
  base=$(git merge-base "$base_ref" HEAD)
  [ "$base" = "$(git rev-parse HEAD)" ] && base=""
else
  echo "no base to compare with; checking the floors only"
fi

work=$(mktemp -d) || exit 1
cleanup() {
  git worktree remove --force "$work/base" >/dev/null 2>&1 || true
  git worktree prune >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT
if [ -n "$base" ]; then
  git worktree add --detach "$work/base" "$base" >&2 || exit 1
fi
# Both sides are judged by one ruler: the base commit's filter set. A PR that
# narrows the filters still measures its head with the old set, so a new
# uncovered file lowers the head figure instead of vanishing. Bases that
# predate the filters file fall back to the head copy.
if [ -n "$base" ] && git show "$base:scripts/coverage-filters.sh" >"$work/filters.sh" 2>/dev/null; then
  :
else
  cp "$root/scripts/coverage-filters.sh" "$work/filters.sh" || exit 1
fi

measure_core() {
  local tree=$1 build=$2 out=$3 filters=$4
  R2S_BUILD_DIR=$build bash "$tree/scripts/core-test.sh" coverage >&2 || return 1
  gcovr_tree "$tree" "$out" "$build/core/coverage" "$filters" CORE_FILTER || return 1
}

# One gcovr call for every measurement. gcov resolves the relative paths
# CMake records against the process directory, so this runs from the measured
# tree as core-test.sh does. The filter set comes from the named file, one
# ruler for both sides (see above); an empty ruler is a hard error, never an
# unfiltered run.
gcovr_tree() {
  local tree=$1 out=$2 obj=$3 filter_src=$4 array=$5
  # shellcheck source=/dev/null
  source "$filter_src"
  declare -p "$array" >/dev/null 2>&1 || { echo "coverage: $filter_src defines no $array" >&2; return 1; }
  local -n wanted=$array
  [ "${#wanted[@]}" -gt 0 ] || { echo "coverage: $filter_src defines no $array" >&2; return 1; }
  # --config /dev/null keeps gcovr from loading gcovr.cfg, gcovr.toml or a
  # [tool.gcovr] section out of the measured tree: only the pinned file rules.
  # --exclude-throw-branches drops the compiler-generated exception edges of
  # calls that never throw in tests, so core and bridge stay comparable. C has
  # no throw edges, so the core figures do not move.
  ( cd "$tree" && gcovr --config /dev/null --root "$tree" "${wanted[@]}" --object-directory "$obj" \
    --exclude-unreachable-branches --exclude-throw-branches --json-summary -o "$out" --print-summary "$obj" >&2 )
}

measure_bridge() {
  local tree=$1 workspace=$2 out=$3 filters=$4
  : "${ROS_DISTRO:?ROS_DISTRO is not set: run it in a ROS container}"
  if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/$ROS_DISTRO/setup.bash" ]; then
    # shellcheck source=/dev/null
    set +u
    source "/opt/ros/$ROS_DISTRO/setup.bash"
    set -u
  fi
  mkdir -p "$workspace" || return 1
  colcon --log-base "$workspace/log" build \
    --base-paths "$tree/bridge" \
    --build-base "$workspace/build" \
    --install-base "$workspace/install" \
    --event-handlers console_direct+ \
    --cmake-args -DCMAKE_CXX_FLAGS="--coverage -O0 -g" -DCMAKE_C_FLAGS="--coverage -O0 -g" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage" -DCMAKE_SHARED_LINKER_FLAGS="--coverage" >&2 || return 1
  # shellcheck source=/dev/null
  set +u
  source "$workspace/install/setup.bash"
  set -u
  colcon --log-base "$workspace/log" test \
    --base-paths "$tree/bridge" \
    --build-base "$workspace/build" \
    --install-base "$workspace/install" \
    --event-handlers console_direct+ \
    --return-code-on-test-failure >&2 || return 1
  # CMake's compiler probes leave stubs gcov cannot resolve, and gcovr reads
  # them before filtering. They are build-system internals, not bridge code.
  find "$workspace/build" -type d -name 'CompilerId*' -prune -exec rm -rf {} + >&2 || return 1
  gcovr_tree "$tree" "$out" "$workspace/build" "$filters" BRIDGE_FILTER || return 1
}

gate_component() {
  local component=$1 head_out=$2 base_out=$3
  # A drop against base always fails, for both components.
  local advisory=()
  if [ -n "$base" ]; then
    python3 tools/coverage_gate.py --component "$component" "${advisory[@]}" --head "$head_out" --base "$base_out"
  else
    python3 tools/coverage_gate.py --component "$component" "${advisory[@]}" --head "$head_out"
  fi
}

echo "== coverage: core on HEAD"
measure_core "$root" "$work/head" "$work/core-head.json" "$work/filters.sh" || exit 1
echo "== coverage: bridge on HEAD"
measure_bridge "$root" "$work/head/bridge" "$work/bridge-head.json" "$work/filters.sh" || exit 1
if [ -n "$base" ]; then
  echo "== coverage: core on $base"
  measure_core "$work/base" "$work/base-build" "$work/core-base.json" "$work/filters.sh" || exit 1
  echo "== coverage: bridge on $base"
  measure_bridge "$work/base" "$work/base-build/bridge" "$work/bridge-base.json" "$work/filters.sh" || exit 1
fi

failed=0
gate_component core "$work/core-head.json" "$work/core-base.json" || failed=1
gate_component bridge "$work/bridge-head.json" "$work/bridge-base.json" || failed=1
exit $failed
