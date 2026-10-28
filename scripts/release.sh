#!/usr/bin/env bash
# Cut a release: next version from the commits (git-cliff and the [bump] rules
# in cliff.toml), VERSION, the generator version files and CHANGELOG.md
# updated, the version-stamped generated files refreshed with make regen, one
# commit. cut-release.yml runs this in the dev image, then opens the release
# PR; the tag follows its merge. Only feat, fix, perf, refactor, revert or
# breaking commits make a release: docs, tests, CI and chores alone are
# nothing to release.
set -euo pipefail
usage() { echo "usage: release.sh [--dry-run] [--bump auto|patch|minor|major]"; exit 2; }
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dry=0
bump=auto
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) dry=1 ;;
    --bump)
      [ $# -ge 2 ] || usage
      case $2 in auto|patch|minor|major) bump=$2 ;; *) usage ;; esac
      shift ;;
    *) usage ;;
  esac
  shift
done
command -v git-cliff >/dev/null || { echo "git-cliff not found: run this in the dev image (make shell)"; exit 1; }
cd "$(git rev-parse --show-toplevel)"
cliff=(git-cliff --config "$root/cliff.toml")

last=$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null || true)
range=${last:+$last..}HEAD
subjects=$(git log --format=%s "$range")
releasable='^(feat|fix|perf|refactor|revert)(\(|!|:)|^[a-z]+(\([^)]*\))?!:'
if ! grep -qE "$releasable" <<<"$subjects" && { [ "$bump" = auto ] || [ -z "$subjects" ]; }; then
  echo "nothing to release since ${last:-the start}: no feat, fix, perf, refactor, revert or breaking commits"
  exit 1
fi

if [ "$bump" = auto ]; then
  next=$("${cliff[@]}" --bumped-version)
else
  next=$("${cliff[@]}" --bump "$bump" --bumped-version)
fi
next=${next#v}
echo "next version: $next"
if [ "$dry" = 1 ]; then
  "${cliff[@]}" --unreleased --tag "v$next"
  exit 0
fi
python3 "$root/scripts/set_version.py" "$next" "$PWD"
make -C "$PWD" regen
"${cliff[@]}" --tag "v$next" -o CHANGELOG.md
git add VERSION CHANGELOG.md gen/r2s_gen/version.py gen/package.xml bridge/r2s_bridge/package.xml \
  bridge/r2s_bridge_tests/package.xml \
  gen/tests/golden/bundle/library.properties gen/tests/golden/bundle/r2s_bundled.c \
  gen/tests/golden/bundle/r2s_bundled.h gen/tests/golden/bundle/ros2serial.h \
  gen/tests/golden/descriptors/float32_twist.c gen/tests/golden/topics/posix.c gen/tests/golden/topics/posix.h \
  gen/tests/golden/types/float32.h gen/tests/golden/types/header.h gen/tests/golden/types/joint_state.h \
  gen/tests/golden/types/twist.h examples/arduino_minimal/lib/library.properties \
  examples/arduino_minimal/lib/src/r2s_generated.c examples/arduino_minimal/lib/src/r2s_generated.h \
  examples/cmake_minimal/lib/CMakeLists.txt examples/cmake_minimal/lib/r2s_generated.c \
  examples/cmake_minimal/lib/r2s_generated.h ports/posix/src/r2s_generated.c ports/posix/src/r2s_generated.h
git commit -q -m "chore(release): v$next"
echo "committed chore(release): v$next"
