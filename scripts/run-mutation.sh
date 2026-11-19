#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Nightly mutation testing: Mull over the core and the ROS-free bridge
# harness, mutmut over the generator. Runs inside the docker/mutation
# image, never in a pull request check, and always ends with a triage
# report in $R2S_MUTATION_OUT. A measured score below its floor in
# tools/robustness.yaml fails the run once the report is written, so the
# scheduled failure reporter files it. Raw tool outputs land next to the
# normalized inputs, so a wrong report bisects to the boundary instead of
# the triage script. Usage, from the repository root:
#   R2S_MUTATION_OUT=mutation-report bash scripts/run-mutation.sh
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
out=${R2S_MUTATION_OUT:-"$root/mutation-report"}
# The report directory is settled before any tool runs. Mull writes its
# reports only after the last mutant, so an unusable directory used to
# surface minutes later as an error naming a path nobody passed.
if ! mkdir -p "$out" 2>/dev/null || [ ! -d "$out" ] || [ ! -w "$out" ]; then
  echo "run-mutation.sh: report directory is not usable: $out" >&2
  exit 2
fi
# Absolute from here on: the bridge Mull loop runs in mutation/bridge, so
# every report, log and build path must survive that directory change.
out=$(cd "$out" && pwd) || exit 2
need() {
  command -v "$1" >/dev/null 2>&1 && return 0
  echo "run-mutation.sh: $1 not found: run inside the docker/mutation image" >&2
  return 1
}
need mull-runner && need mull-reporter && need mutmut && need cmake && need ninja || exit 2
# The bridge harness links gtest and yaml-cpp, which only the mutation
# image carries: headers are enough to check, cmake names the rest.
[ -f /usr/include/gtest/gtest.h ] && [ -f /usr/include/yaml-cpp/yaml.h ] || {
  echo "run-mutation.sh: gtest and yaml-cpp headers not found: run inside the docker/mutation image" >&2
  exit 1
}

# The core, built with the Mull frontend for LLVM 18: mutants are injected
# at IR level, so the object files must carry the frontend's instrumentation.
# The C++ check compiles the same headers, so both frontends carry the flag.
build="$out/core-build"
plugin="-O0 -fpass-plugin=/usr/lib/mull-ir-frontend-18 -g -grecord-command-line"
cmake -S core -B "$build" -G Ninja -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_C_FLAGS="$plugin" -DCMAKE_CXX_FLAGS="$plugin" \
  >"$out/core-configure.log" 2>&1 || { cat "$out/core-configure.log"; exit 1; }
cmake --build "$build" >"$out/core-build.log" 2>&1 || { tail -20 "$out/core-build.log"; exit 1; }

# One binary per ctest case group, run from the repository root (the vector
# checks name their files relative to it). The must_fail harnesses fail by
# design, so Mull's baseline run would reject them: they test the Unity
# support macros, not product code, and stay out.
skip="bytes_mismatch|overrun_unnoticed|selftest_mismatch"
binaries=$(ctest --test-dir "$build" -N 2>/dev/null | sed -n 's/^ *Test *#*[0-9][0-9]*: //p' | cut -d. -f1 | sort -u \
  | grep -Ev "^($skip)$" || true)
[ -n "$binaries" ] || { echo "run-mutation.sh: no test binaries found in $build" >&2; exit 1; }
# Mull discovers its configuration as mull.yml in the working directory,
# which is the repository root here (the vector checks name their files
# relative to it), so the file lives at the root and not beside the image.
# Mull joins --report-dir and --report-name itself, so the name stays bare:
# an absolute name makes the joined path relative to the working directory.
rm -f "$out/mull.sqlite"
for binary in $binaries; do
  path=$(find "$build" -name "$binary" -type f -executable | head -1)
  [ -n "$path" ] || { echo "run-mutation.sh: binary not built: $binary" >&2; exit 1; }
  mull-runner --allow-surviving --reporters SQLite --report-dir "$out" --report-name mull "$path" \
    >"$out/mull-$binary.log" 2>&1 || { tail -20 "$out/mull-$binary.log"; exit 1; }
done
mull-reporter --allow-surviving --reporters Elements --report-dir "$out" --report-name mull "$out/mull.sqlite" \
  >"$out/mull-reporter.log" 2>&1 || { tail -20 "$out/mull-reporter.log"; exit 1; }
python3 - "$out" <<'EOF'
import glob
import json
import os
import sys

out = sys.argv[1]
candidates = sorted(glob.glob(os.path.join(out, "*.json")))
if not candidates:
    sys.exit("run-mutation.sh: mull-reporter wrote no Elements report")
elements = json.load(open(candidates[0]))
surviving = {"Survived", "Timeout", "NoCoverage", "RuntimeError"}
mutants = []
for path, data in elements.get("files", {}).items():
    for mutant in data.get("mutants", []):
        status = mutant.get("status")
        if status == "Ignored":
            continue
        if status == "Killed":
            outcome = "killed"
        elif status in surviving:
            outcome = "survived"
        else:
            sys.exit(f"run-mutation.sh: unknown Mull status: {status}")
        line = mutant["location"]["start"]["line"]
        mutator = mutant.get("mutatorName", "unknown")
        mutants.append(
            {
                "id": f"core:{path}:{line}:{mutator}",
                "file": path,
                "line": line,
                "mutator": mutator,
                "outcome": outcome,
            }
        )
json.dump({"component": "core", "mutants": mutants}, open(os.path.join(out, "mull.json"), "w"), indent=2)
print(f"mull: {len(mutants)} mutants from {candidates[0]}")
EOF

# The bridge's ROS-free logic classes, from mutation/bridge: Mull runs one
# binary per harness test group, the same shape as the core loop. The run
# happens with mutation/bridge as the working directory, so its own
# mull.yml (which keeps the core sources and the system headers out of the
# bridge score) applies instead of the root one. The rclcpp shell around
# these classes has no harness yet and stays out of the score.
bridge_build="$out/bridge-build"
cmake -S mutation/bridge -B "$bridge_build" -G Ninja -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_C_FLAGS="$plugin" -DCMAKE_CXX_FLAGS="$plugin" \
  >"$out/bridge-configure.log" 2>&1 || { cat "$out/bridge-configure.log"; exit 1; }
cmake --build "$bridge_build" >"$out/bridge-build.log" 2>&1 || { tail -20 "$out/bridge-build.log"; exit 1; }
bridge_binaries=$(ctest --test-dir "$bridge_build" -N 2>/dev/null | sed -n 's/^ *Test *#*[0-9][0-9]*: //p' | cut -d. -f1 | sort -u || true)
[ -n "$bridge_binaries" ] || { echo "run-mutation.sh: no test binaries found in $bridge_build" >&2; exit 1; }
rm -f "$out/mull-bridge.sqlite"
(
  cd mutation/bridge || exit 2
  for binary in $bridge_binaries; do
    path=$(find "$bridge_build" -name "$binary" -type f -executable | head -1)
    [ -n "$path" ] || { echo "run-mutation.sh: binary not built: $binary" >&2; exit 1; }
    mull-runner --allow-surviving --reporters SQLite --report-dir "$out" --report-name mull-bridge "$path" \
      >"$out/mull-bridge-$binary.log" 2>&1 || { tail -20 "$out/mull-bridge-$binary.log"; exit 1; }
  done
  mull-reporter --allow-surviving --reporters Elements --report-dir "$out" --report-name mull-bridge \
    "$out/mull-bridge.sqlite" \
    >"$out/mull-bridge-reporter.log" 2>&1 || { tail -20 "$out/mull-bridge-reporter.log"; exit 1; }
) || exit 1
python3 - "$out" <<'EOF'
import glob
import json
import os
import sys

out = sys.argv[1]
candidates = sorted(glob.glob(os.path.join(out, "mull-bridge*.json")))
if not candidates:
    sys.exit("run-mutation.sh: mull-reporter wrote no bridge Elements report")
elements = json.load(open(candidates[0]))
surviving = {"Survived", "Timeout", "NoCoverage", "RuntimeError"}
mutants = []
for path, data in elements.get("files", {}).items():
    for mutant in data.get("mutants", []):
        status = mutant.get("status")
        if status == "Ignored":
            continue
        if status == "Killed":
            outcome = "killed"
        elif status in surviving:
            outcome = "survived"
        else:
            sys.exit(f"run-mutation.sh: unknown Mull status: {status}")
        line = mutant["location"]["start"]["line"]
        mutator = mutant.get("mutatorName", "unknown")
        mutants.append(
            {
                "id": f"bridge:{path}:{line}:{mutator}",
                "file": path,
                "line": line,
                "mutator": mutator,
                "outcome": outcome,
            }
        )
json.dump({"component": "bridge", "mutants": mutants}, open(os.path.join(out, "bridge.json"), "w"), indent=2)
print(f"mull-bridge: {len(mutants)} mutants from {candidates[0]}")
EOF

# The generator: mutmut runs the gen suite once per mutant, from a workspace
# shaped the way its path-to-key mapping needs. Only the surviving and
# otherwise undecided mutants need locating: mutmut show prints the diff,
# whose first hunk names the line.
host_dir="$out/host-lib"
bash scripts/build-host-lib.sh "$host_dir" >"$out/host-build.log" 2>&1 || { tail -20 "$out/host-build.log"; exit 1; }
export R2S_HOST_LIB_DIR="$host_dir"
# The workspace sits outside the checkout: pytest walks up from mutants/ and
# loads every conftest it finds, so one staged inside the tree would load the
# repository's own conftest beside the copy and register its options twice.
work=$(mktemp -d "${TMPDIR:-/tmp}/r2s-mutmut-XXXXXX") || exit 2
# A failing step exits from inside the subshell, so the cleanup needs to run
# on every exit path rather than after the last command.
trap 'rm -rf "$work"' EXIT
bash scripts/stage-mutmut-workspace.sh "$root" "$work" >"$out/mutmut-stage.log" 2>&1 || {
  cat "$out/mutmut-stage.log"
  exit 1
}
(
  cd "$work" || exit 2
  mutmut run >"$out/mutmut-run.log" 2>&1 || { tail -20 "$out/mutmut-run.log"; exit 1; }
  mutmut export-cicd-stats >"$out/mutmut-stats.log" 2>&1 || { tail -5 "$out/mutmut-stats.log"; exit 1; }
  cp mutants/mutmut-cicd-stats.json "$out/mutmut-cicd-stats.json" || exit 1
  # --all takes a value in mutmut 3.8.1, so a bare --all exits 2.
  mutmut results --all true >"$out/mutmut-results.log" 2>&1 || { tail -5 "$out/mutmut-results.log"; exit 1; }
  python3 - "$out" <<'EOF'
import json
import os
import re
import subprocess
import sys

from mutmut.stats import status_by_exit_code
from mutmut.mutation.data import SourceFileMutationData
from mutmut.utils.file_utils import walk_mutatable_files

out = sys.argv[1]
mutants = []
for path in walk_mutatable_files():
    data = SourceFileMutationData(path=path)
    data.load()
    # The run happens in the staged workspace, where the package sits at the
    # top; every report and triage entry names the path in the checkout.
    repo_path = str(path)
    if repo_path.startswith("r2s_gen/"):
        repo_path = f"gen/{repo_path}"
    for key, code in data.exit_code_by_key.items():
        status = status_by_exit_code[code]
        if status in ("killed", "skipped", "not checked"):
            outcome = "killed" if status == "killed" else None
            if outcome is None:
                continue
            line, mutator = 0, key
        elif status in ("survived", "timeout", "no tests", "suspicious", "segfault"):
            outcome = "survived"
            shown = subprocess.run(["mutmut", "show", key], capture_output=True, text=True, check=False)
            if shown.returncode != 0:
                sys.exit(f"run-mutation.sh: mutmut show failed for {key}: {shown.stderr.strip()}")
            hunks = re.findall(r"^@@ -\d+(?:,\d+)? \+(\d+)", shown.stdout, re.M)
            line = int(hunks[0]) if hunks else 0
            mutator = key
        else:
            sys.exit(f"run-mutation.sh: unknown mutmut status: {status} for {key}")
        mutants.append(
            {
                "id": f"gen:{repo_path}:{line}:{mutator}",
                "file": repo_path,
                "line": line,
                "mutator": mutator,
                "outcome": outcome,
            }
        )
json.dump({"component": "gen", "mutants": mutants}, open(os.path.join(out, "mutmut.json"), "w"), indent=2)
print(f"mutmut: {len(mutants)} mutants")
EOF
) || exit 1

python3 scripts/mutation_triage.py --mull "$out/mull.json" --mutmut "$out/mutmut.json" \
  --bridge "$out/bridge.json" \
  --equivalent mutation/equivalent.yaml --out "$out/report.md" --gate
echo "report: $out/report.md"
