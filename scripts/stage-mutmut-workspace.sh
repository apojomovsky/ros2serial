#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Stages the workspace mutmut runs in. Two facts fix its shape. mutmut names
# a mutant after the source path it was given, so the package has to sit at
# the top of that directory for its keys to match the name the tests
# import. And it runs pytest from mutants/, so the suite and every tree it
# resolves through the root must be staged there.
# Usage: stage-mutmut-workspace.sh <repository root> <workspace>
set -uo pipefail
src=$(cd "${1:?usage: stage-mutmut-workspace.sh <root> <workspace>}" && pwd) || exit 2
dst=${2:?usage: stage-mutmut-workspace.sh <root> <workspace>}
[ -d "$src/gen/tests" ] || {
  echo "stage-mutmut-workspace.sh: no gen/tests under $src" >&2
  exit 1
}
[ -d "$src/gen/r2s_gen" ] || {
  echo "stage-mutmut-workspace.sh: no gen/r2s_gen under $src" >&2
  exit 1
}

rm -rf "$dst" || exit 2
mkdir -p "$dst" || exit 2
# The workspace lives under the report directory, which lives in the tree,
# so the copy must step over both; tar sees a tree that changes under it and
# fails otherwise. The report directory holds no input: it is this run's
# output, build trees and logs included.
exclude_dst=()
for rel in "$(realpath --relative-to="$src" "$dst" 2>/dev/null || true)" \
           "$(realpath --relative-to="$src" "${R2S_MUTATION_OUT:-$src/mutation-report}" 2>/dev/null || true)"; do
  case "$rel" in
    ''|.|./|/*) continue ;;
    ./*) rel=${rel#./} ;;
  esac
  exclude_dst+=("--exclude=./$rel")
done

# The workspace top is the repository, so a tool that reads its own path
# upward (r2s-arduino does, for the firmware assets) finds the tree. Its
# conftest stays out: pytest loads the conftest of every directory above its
# rootdir, and a second copy beside the staged one registers --shard twice.
tar -C "$src" -cf - \
  --exclude=./.git --exclude=./gen/r2s_gen --exclude=./conftest.py --exclude='__pycache__' \
  "${exclude_dst[@]}" \
  . | tar -C "$dst" -xf - || exit 1
# The package at the top, where its path names the module the tests import.
# mutmut copies and mutates it into mutants/ itself.
cp -a "$src/gen/r2s_gen" "$dst/r2s_gen" || exit 1

# The tree pytest runs in: the same copy, so the suite and every tree it
# resolves through the root are there.
tree="$dst/mutants"
mkdir -p "$tree" || exit 2
tar -C "$dst" -cf - --exclude=./mutants --exclude=./r2s_gen . | tar -C "$tree" -xf - || exit 1
cp -a "$src/conftest.py" "$tree/conftest.py" || exit 1

cat >"$dst/pyproject.toml" <<'TOML'
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The mutation run's own configuration. scripts/stage-mutmut-workspace.sh
# writes it, because the paths differ from the repository's: the package sits
# at the top here, and the tests that install it in a subprocess cannot import
# mutmut's trampoline, so they cannot run under mutation at all.
[tool.mutmut]
source_paths = ["r2s_gen"]
pytest_add_cli_args_test_selection = ["gen/tests"]
pytest_add_cli_args = [
  "--ignore", "gen/tests/test_packaging.py",
  "--ignore", "gen/tests/test_rosidl_plugin.py",
]
TOML
echo "stage-mutmut-workspace.sh: staged $dst"
