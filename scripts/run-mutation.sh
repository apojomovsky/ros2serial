#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Nightly mutation testing: Mull over the core, mutmut over the generator.
# Runs inside the docker/mutation image, never in a pull request check, and
# always ends with a triage report in $R2S_MUTATION_OUT (never a gate: even
# a score below the floor exits 0 once the report is written). Raw tool
# outputs land next to the normalized inputs, so a wrong report bisects to
# the boundary instead of the triage script. Usage, from the repository root:
#   R2S_MUTATION_OUT=mutation-report bash scripts/run-mutation.sh
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
out=${R2S_MUTATION_OUT:-"$root/mutation-report"}
mkdir -p "$out" || exit 2
need() {
  command -v "$1" >/dev/null 2>&1 && return 0
  echo "run-mutation.sh: $1 not found: run inside the docker/mutation image" >&2
  return 1
}
need mull-runner && need mull-reporter && need mutmut && need cmake && need ninja || exit 2

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
binaries=$(ctest --test-dir "$build" -N 2>/dev/null | sed -n 's/^ *Test *[0-9]*: //p' | cut -d. -f1 | sort -u \
  | grep -Ev "^($skip)$" || true)
[ -n "$binaries" ] || { echo "run-mutation.sh: no test binaries found in $build" >&2; exit 1; }
# Mull discovers its configuration as mull.yml in the working directory,
# which is the repository root here (the vector checks name their files
# relative to it), so the file lives at the root and not beside the image.
rm -f "$out/mull.sqlite"
for binary in $binaries; do
  path=$(find "$build" -name "$binary" -type f -executable | head -1)
  [ -n "$path" ] || { echo "run-mutation.sh: binary not built: $binary" >&2; exit 1; }
  mull-runner --allow-surviving --reporters SQLite --report-name "$out/mull" "$path" \
    >"$out/mull-$binary.log" 2>&1 || { tail -20 "$out/mull-$binary.log"; exit 1; }
done
mull-reporter --reporters Elements --report-dir "$out" --report-name mull "$out/mull.sqlite" \
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

# The generator: mutmut runs the gen suite once per mutant from the
# repository root, with the host library the model tests load. Only the
# surviving and otherwise undecided mutants need locating: mutmut show
# prints the diff, whose first hunk names the line.
host_dir="$out/host-lib"
bash scripts/build-host-lib.sh "$host_dir" >"$out/host-build.log" 2>&1 || { tail -20 "$out/host-build.log"; exit 1; }
export R2S_HOST_LIB_DIR="$host_dir"
mutmut run >"$out/mutmut-run.log" 2>&1 || { tail -20 "$out/mutmut-run.log"; exit 1; }
mutmut export-cicd-stats >"$out/mutmut-stats.log" 2>&1 || { tail -5 "$out/mutmut-stats.log"; exit 1; }
cp mutants/mutmut-cicd-stats.json "$out/mutmut-cicd-stats.json"
mutmut results --all >"$out/mutmut-results.log" 2>&1 || { tail -5 "$out/mutmut-results.log"; exit 1; }
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
                "id": f"gen:{path}:{line}:{mutator}",
                "file": str(path),
                "line": line,
                "mutator": mutator,
                "outcome": outcome,
            }
        )
json.dump({"component": "gen", "mutants": mutants}, open(os.path.join(out, "mutmut.json"), "w"), indent=2)
print(f"mutmut: {len(mutants)} mutants")
EOF

python3 scripts/mutation_triage.py --mull "$out/mull.json" --mutmut "$out/mutmut.json" \
  --equivalent mutation/equivalent.yaml --out "$out/report.md"
echo "report: $out/report.md"
