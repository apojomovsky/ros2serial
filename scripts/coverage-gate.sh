#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Coverage gate: fail when a line the pull request changes is uncovered and
# not excluded, or when the whole package sits below the floors in
# docs/01-testing.md. A dip of the whole-package figure against the merge
# base no longer fails: the patch figure judges the changed lines instead.
# The base is still measured by checking it out in a detached worktree and
# running the same measurement there, so a PR cannot relax its own judge
# and no baseline file, secret or external service is needed.
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
  local tree=$1 build=$2 out=$3 detail=$4 filters=$5
  R2S_BUILD_DIR=$build bash "$tree/scripts/core-test.sh" coverage >&2 || return 1
  gcovr_tree "$tree" "$out" "$detail" "$build/core/coverage" "$filters" CORE_FILTER || return 1
}

# One gcovr call for every measurement. gcov resolves the relative paths
# CMake records against the process directory, so this runs from the measured
# tree as core-test.sh does. The filter set comes from the named file, one
# ruler for both sides (see above); an empty ruler is a hard error, never an
# unfiltered run. One call writes both the summary the floors judge and the
# detail the patch check judges, so both figures come from one measurement.
gcovr_tree() {
  local tree=$1 out=$2 detail=$3 obj=$4 filter_src=$5 array=$6
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
    --exclude-unreachable-branches --exclude-throw-branches \
    --json-summary="$out" --json="$detail" --print-summary "$obj" >&2 )
}

measure_bridge() {
  local tree=$1 workspace=$2 out=$3 detail=$4 filters=$5
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
  gcovr_tree "$tree" "$out" "$detail" "$workspace/build" "$filters" BRIDGE_FILTER || return 1
}

measure_gen() {
  local tree=$1 build=$2 out=$3
  local host_dir="$build/host" data="$build/gen-coverage.dat"
  R2S_BUILD_DIR=$build bash "$tree/scripts/build-host-lib.sh" "$host_dir" >&2 || return 1
  # Flags, not the pyproject defaults, define the measured scope, so a base
  # that predates the config still measures the same files on both sides.
  # A fixed hypothesis seed for both sides, so the comparison measures code
  # and not luck (docs/01-testing.md, section 5.2). The value is the gen
  # suite's fixed seed. The suite is derandomized, which takes precedence
  # over the flag; the flag pins bases predating the pin.
  ( cd "$tree" && R2S_HOST_LIB_DIR="$host_dir" coverage run --branch --source=gen/r2s_gen \
    --data-file="$data" -m pytest gen -p no:cacheprovider --hypothesis-seed=20261026 >&2 ) || return 1
  # The raw coverage.py report is both the summary the floors judge and the
  # detail the patch check judges: totals and per-line data in one file.
  ( cd "$tree" && coverage json --data-file="$data" -o "$out" >&2 ) || return 1
}

# The measured paths per component, kept next to the gcovr filter file they
# mirror: a changed line only judges the component whose paths hold it.
# Bridge paths drop the rcl shell the filters also drop, so a change there
# never judges the bridge patch.
patch_diff() {
  local base=$1 pathspec=$2 out=$3 names=$4
  shift 4
  # No rename detection: a rename reads as a delete plus an add, so the name
  # lists and the added-line counts below agree on plain paths.
  git diff --no-renames -U0 "$base" HEAD -- "$pathspec" "$@" >"$out" || return 1
  git diff --no-renames --name-only "$base" HEAD -- "$pathspec" "$@" | sort -u >"$names" || return 1
}

gate_component() {
  local component=$1 head_out=$2 base_out=${3:-} detail=${4:-} diff=${5:-} names=${6:-}
  # The patch judges the changed lines, the floors judge the whole package.
  # With no base the floors alone judge, so a missing base never passes
  # silently and the patch has nothing to diff against.
  local advisory=()
  if [ -n "$base" ]; then
    local measured=()
    while IFS= read -r line; do
      [ -n "$line" ] && measured+=(--measured "$line")
    done <"$names"
    python3 tools/coverage_gate.py --component "$component" "${advisory[@]}" --head "$head_out" \
      --base "$base_out" --detail "$detail" --diff "$diff" --repo "$root" "${measured[@]}"
  else
    python3 tools/coverage_gate.py --component "$component" "${advisory[@]}" --head "$head_out"
  fi
}

# Changed lines outside every component's measured paths are ignored, and
# said so on one line: generated files, docs and tooling never judge a patch.
report_ignored() {
  git diff --no-renames --name-only "$base" HEAD -- . | sort -u >"$work/changed.names" || return 1
  cat "$work"/patch-*.names | sort -u >"$work/measured.names" || return 1
  comm -23 "$work/changed.names" "$work/measured.names" >"$work/ignored.names" || return 1
  if [ ! -s "$work/ignored.names" ]; then
    echo "coverage: patch: all changed lines are in measured code"
    return 0
  fi
  local lines=0 files=0 list=""
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    files=$((files + 1))
    list="$list $name"
  done <"$work/ignored.names"
  lines=$(git diff --no-renames -U0 --numstat "$base" HEAD -- . | while IFS=$'\t' read -r added _ name; do
    if grep -qxF "$name" "$work/ignored.names" && [ "$added" != "-" ]; then echo "$added"; fi
  done | awk '{s+=$1} END {print s+0}')
  echo "coverage: patch: ignoring $lines changed lines in $files files outside measured code:$list"
}

echo "== coverage: core on HEAD"
measure_core "$root" "$work/head" "$work/core-head.json" "$work/core-head-detail.json" "$work/filters.sh" || exit 1
echo "== coverage: bridge on HEAD"
measure_bridge "$root" "$work/head/bridge" "$work/bridge-head.json" "$work/bridge-head-detail.json" \
  "$work/filters.sh" || exit 1
echo "== coverage: gen on HEAD"
measure_gen "$root" "$work/head" "$work/gen-head.json" || exit 1
if [ -n "$base" ]; then
  echo "== coverage: core on $base"
  measure_core "$work/base" "$work/base-build" "$work/core-base.json" "$work/core-base-detail.json" \
    "$work/filters.sh" || exit 1
  echo "== coverage: bridge on $base"
  measure_bridge "$work/base" "$work/base-build/bridge" "$work/bridge-base.json" \
    "$work/bridge-base-detail.json" "$work/filters.sh" || exit 1
  echo "== coverage: gen on $base"
  measure_gen "$work/base" "$work/base-build" "$work/gen-base.json" || exit 1
  echo "== coverage: changed lines since $base"
  patch_diff "$base" core/src/ "$work/patch-core.diff" "$work/patch-core.names" || exit 1
  patch_diff "$base" bridge/r2s_bridge/src "$work/bridge-src.diff" "$work/bridge-src.names" || exit 1
  patch_diff "$base" bridge/r2s_bridge/include "$work/bridge-inc.diff" "$work/bridge-inc.names" \
    || exit 1
  # The combined bridge diff may hold sections the rcl shell exclusions drop;
  # the names list below gates them out, so they never judge the patch.
  cat "$work/bridge-src.diff" "$work/bridge-inc.diff" >"$work/patch-bridge.diff" || exit 1
  # grep exits 1 when it drops every line; that is the normal no-change case.
  { cat "$work/bridge-src.names" "$work/bridge-inc.names" \
    | grep -v -e 'bridge/r2s_bridge/src/bridge\.cpp$' -e 'bridge/r2s_bridge/src/main\.cpp$' \
      -e 'bridge/r2s_bridge/include/r2s_bridge/bridge\.hpp$' || true; } \
    | sort -u >"$work/patch-bridge.names" || exit 1
  patch_diff "$base" gen/r2s_gen/ "$work/patch-gen.diff" "$work/patch-gen.names" || exit 1
fi

failed=0
if [ -n "$base" ]; then
  report_ignored || failed=1
  gate_component core "$work/core-head.json" "$work/core-base.json" \
    "$work/core-head-detail.json" "$work/patch-core.diff" "$work/patch-core.names" || failed=1
  gate_component bridge "$work/bridge-head.json" "$work/bridge-base.json" \
    "$work/bridge-head-detail.json" "$work/patch-bridge.diff" "$work/patch-bridge.names" || failed=1
  gate_component gen "$work/gen-head.json" "$work/gen-base.json" \
    "$work/gen-head.json" "$work/patch-gen.diff" "$work/patch-gen.names" || failed=1
else
  gate_component core "$work/core-head.json" "$work/core-base.json" || failed=1
  gate_component bridge "$work/bridge-head.json" "$work/bridge-base.json" || failed=1
  gate_component gen "$work/gen-head.json" "$work/gen-base.json" || failed=1
fi
exit $failed
