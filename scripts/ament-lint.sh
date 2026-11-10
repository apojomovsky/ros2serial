#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Advisory ament_lint_common run over the ROS packages: builds with the lint
# tests enabled, runs only them, prints the per-linter baseline, and exits
# nonzero only when a cleaned area reports findings or nothing ran at all.
# The CI gate stays non-blocking through continue-on-error.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
command -v colcon >/dev/null 2>&1 || { echo "colcon not found: run it in a ROS container" >&2; exit 1; }
: "${ROS_DISTRO:?ROS_DISTRO is not set: run it in a ROS container}"
if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/$ROS_DISTRO/setup.bash" ]; then
  # shellcheck source=/dev/null
  set +u
  source "/opt/ros/$ROS_DISTRO/setup.bash"
  set -u
fi
[ "$#" -eq 0 ] || { echo "usage: scripts/ament-lint.sh" >&2; exit 2; }
workspace=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/ament-lint
# One lock for the lint workspace, held for the entire run: a direct run and
# a suite run of one tree must not write the same binaries.
if [ -z "${R2S_AMENT_LINT_LOCKED:-}" ]; then
  mkdir -p "$workspace" || exit 1
  exec env R2S_AMENT_LINT_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$workspace/.lock" \
    bash "$root/scripts/ament-lint.sh"
fi
mkdir -p "$workspace" || exit 1
echo "== ament-lint: $ROS_DISTRO (advisory; cpplint must report zero)"

# The lint test names ament_lint_common registers, so colcon runs only them
# and never the unit tests.
linters="copyright cppcheck cpplint flake8 lint_cmake pep257 uncrustify xmllint"
cmake_pkgs="r2s_bridge r2s_rosidl"
# r2s_rosidl is named beside gen the way the farm check does: a plain gen
# base path would hide the nested package from colcon.
# shellcheck disable=SC2206
base_paths=(bridge/r2s_bridge gen gen/r2s_rosidl)
colcon --log-base "$workspace/log" build \
  --base-paths "${base_paths[@]}" \
  --build-base "$workspace/build" \
  --install-base "$workspace/install" \
  --cmake-args -DR2S_AMENT_LINT=ON \
  --event-handlers console_direct+ || exit 1

pattern=$(printf '%s' "$linters" | tr ' ' '|')
# shellcheck disable=SC2086
colcon --log-base "$workspace/log" test \
  --base-paths "${base_paths[@]}" \
  --build-base "$workspace/build" \
  --install-base "$workspace/install" \
  --packages-select $cmake_pkgs \
  --ctest-args -R "^(${pattern})$" \
  --event-handlers console_direct+ || true
colcon --log-base "$workspace/log" test-result \
  --test-result-base "$workspace/build" --all --verbose || true

# The generator's linters run straight from its tree: ament_python has no
# CMake for the lint hook, so pytest runs one file per linter.
export R2S_AMENT_LINT=1
mkdir -p "$workspace/gen-results"
for linter in flake8 pep257 copyright xmllint; do
  (cd gen && python3 -m pytest "tests/test_${linter}.py" --junitxml="$workspace/gen-results/${linter}.xml" -q) || true
done

# Baseline table from the xunit files: failing adds failures and errors,
# checked subtracts the skipped, a missing file reports dashes. cpplint on
# the bridge must report zero: the remaining linters stay advisory until
# their own cleanup tickets land. copyright is the known divergence: the
# tree carries the SPDX two-liner by decision (#549), while ament_copyright
# only accepts its fixed MIT permission text, so the table keeps showing
# the count without failing the run.
echo "== ament-lint baseline (advisory, #508; cleaned areas must report zero)"
echo "# package linter failing checked"
clean=0
attr() {
  grep -m1 -o "$2=\"[0-9]*\"" "$1" 2>/dev/null | grep -o "[0-9]*" || true
}
found=0
row() {
  local file=$1 pkg=$2 linter=$3
  local tests failures errors skipped
  if [ ! -f "$file" ]; then
    echo "$pkg $linter - -"
    return 0
  fi
  found=$((found + 1))
  tests=$(attr "$file" tests)
  failures=$(attr "$file" failures)
  errors=$(attr "$file" errors)
  skipped=$(attr "$file" skipped)
  failing=$((${failures:-0} + ${errors:-0}))
  echo "$pkg $linter $failing $((${tests:-0} - ${skipped:-0}))"
  case "$linter" in
    cpplint)
      if [ "$failing" -ne 0 ]; then
        echo "ament-lint: $pkg $linter reports $failing findings, expected zero (#511)" >&2
        clean=1
      fi
      ;;
  esac
}
for pkg in $cmake_pkgs; do
  # shellcheck disable=SC2086
  for linter in $linters; do
    row "$workspace/build/$pkg/test_results/$pkg/${linter}.xunit.xml" "$pkg" "$linter"
  done
done
for linter in flake8 pep257 copyright xmllint; do
  row "$workspace/gen-results/${linter}.xml" r2s_gen "$linter"
done
# The cleaned areas fail here, never in the CI gate: the workflow keeps
# continue-on-error until the remaining linters are clean. A silent no-run
# still fails loudly instead of reporting an empty baseline.
if [ "$found" -eq 0 ]; then
  echo "ament-lint: no lint results were produced; the linters did not run" >&2
  exit 1
fi
exit "$clean"
