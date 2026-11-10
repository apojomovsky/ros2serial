#!/usr/bin/env bash
# Advisory documentation build: the #512 Doxygen-form gate on the core, a
# doxygen warning count per Doxyfile, and rosdoc2 build per ROS package.
# The core count fails the run; the bridge count stays advisory until #544
# converts the bridge headers, then gates at zero like the core. CI runs
# this through the rosdoc2 workflow with continue-on-error, so red stays
# visible without blocking the merge.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
[ "$#" -eq 0 ] || { echo "usage: scripts/rosdoc2.sh" >&2; exit 2; }

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "scripts/rosdoc2.sh: $1 not found: install it first ($2)" >&2
    exit 1
  }
}
need doxygen "apt-get install doxygen"
need rosdoc2 "pip install rosdoc2==$(grep ^ROSDOC2_VERSION= ci/versions.env | cut -d= -f2)"

workspace=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/rosdoc2
mkdir -p "$workspace" || exit 1

echo "== rosdoc2: the Doxygen-form gate on the core"
python3 scripts/doxygen_check.py || exit 1

fail=0
run_doxygen() {
  local dir=$1 mode=$2 log=$workspace/doxygen-$(echo "$1" | tr / -).log
  (cd "$dir" && doxygen Doxyfile >"$log" 2>&1)
  local status=$?
  local count
  count=$(grep -c "warning:" "$log" || true)
  echo "doxygen $dir: $count warning(s)"
  rm -rf "$dir/doc_output"
  if [ "$status" -ne 0 ]; then
    echo "doxygen $dir failed; see $log" >&2
    return 1
  fi
  if [ "$mode" = zero ] && [ "$count" -ne 0 ]; then
    echo "doxygen $dir warns on $count public symbol(s); see $log" >&2
    return 1
  fi
  return 0
}

run_doxygen core zero || fail=1
run_doxygen bridge/r2s_bridge report || fail=1

for pkg in bridge/r2s_bridge bridge/r2s_bridge_tests gen gen/r2s_rosidl; do
  name=$(basename "$pkg")
  echo "== rosdoc2: build $pkg"
  rosdoc2 build --package-path "$pkg" \
    --output-directory "$workspace/output/$name" \
    --doc-build-directory "$workspace/build/$name" || fail=1
done

exit "$fail"
