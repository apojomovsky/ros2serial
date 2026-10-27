"""The coverage gate judges a fixture PR: fail on a drop, pass on equal figures.

The fixture repo carries the real scripts/coverage-gate.sh, the real
scripts/coverage-filters.sh and the real tools/coverage_gate.py, with stub
builds (core-test.sh, colcon) and a stub gcovr that reports the figures its
environment names. Nothing here compiles or measures real code.
"""

import os
import shutil
import stat
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

GCOVR_STUB = """#!/usr/bin/env bash
set -uo pipefail
root=""
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    --root) root="$2"; shift 2 ;;
    -o) out="$2"; shift 2 ;;
    *) shift ;;
  esac
done
if [[ "$root" == */base ]]; then figs=$R2S_FIG_BASE; else figs=$R2S_FIG_HEAD; fi
set -- $figs
python3 -c "import json; json.dump({'line_covered': $1, 'line_total': 100, \
  'branch_covered': $2, 'branch_total': 100}, open('$out', 'w'))"
"""
COLCON_STUB = """#!/usr/bin/env bash
set -uo pipefail
want=""
install=""
for arg in "$@"; do
  if [ "$want" = dir ]; then mkdir -p "$arg"; install="$arg"; want=""; fi
  if [ "$arg" = --build-base ] || [ "$arg" = --install-base ]; then want=dir; fi
done
touch "$install/setup.bash"
"""


def fixture(tmp_path: Path) -> tuple[Path, dict[str, str]]:
    repo = tmp_path / "repo"
    (repo / "scripts").mkdir(parents=True)
    (repo / "tools").mkdir()
    (repo / "bridge").mkdir()
    shutil.copy(ROOT / "scripts" / "coverage-gate.sh", repo / "scripts" / "coverage-gate.sh")
    shutil.copy(ROOT / "scripts" / "coverage-filters.sh", repo / "scripts" / "coverage-filters.sh")
    shutil.copy(ROOT / "tools" / "coverage_gate.py", repo / "tools" / "coverage_gate.py")
    (repo / "scripts" / "core-test.sh").write_text("#!/usr/bin/env bash\nexit 0\n")
    (repo / "bridge" / "dummy.txt").write_text("bridge sources live here on a real tree\n")
    for name in ("scripts/coverage-gate.sh", "scripts/core-test.sh"):
        path = repo / name
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "gcovr").write_text(GCOVR_STUB)
    (bindir / "colcon").write_text(COLCON_STUB)
    for name in ("gcovr", "colcon"):
        path = bindir / name
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
    env = {"PATH": "/usr/bin:/bin", "HOME": str(tmp_path)}
    subprocess.run([*git, "init", "-b", "master"], cwd=repo, env=env, check=True, capture_output=True)
    subprocess.run([*git, "add", "-A"], cwd=repo, env=env, check=True, capture_output=True)
    subprocess.run([*git, "commit", "-m", "base"], cwd=repo, env=env, check=True, capture_output=True)
    subprocess.run([*git, "checkout", "--detach"], cwd=repo, env=env, check=True, capture_output=True)
    subprocess.run([*git, "commit", "--allow-empty", "-m", "head"], cwd=repo, env=env, check=True, capture_output=True)
    run_env = {"PATH": str(bindir), "HOME": str(tmp_path)}
    return repo, run_env


def run_gate(repo: Path, run_env: dict[str, str], head: str, base: str) -> subprocess.CompletedProcess:
    env = {
        **os.environ,
        "PATH": f"{run_env['PATH']}{os.pathsep}{os.environ.get('PATH', '/usr/bin:/bin')}",
        "HOME": run_env["HOME"],
        "AMENT_PREFIX_PATH": "/stub",
        "COMMIT_RANGE": "master..HEAD",
        "R2S_FIG_HEAD": head,
        "R2S_FIG_BASE": base,
    }
    return subprocess.run(["bash", "scripts/coverage-gate.sh"], cwd=repo, env=env, capture_output=True, text=True)


def commit_on_head(repo: Path, run_env: dict[str, str], name: str, text: str) -> None:
    (repo / name).write_text(text)
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
    env = {"PATH": "/usr/bin:/bin", "HOME": run_env["HOME"]}
    subprocess.run([*git, "add", "-A"], cwd=repo, env=env, check=True, capture_output=True)
    subprocess.run([*git, "commit", "-m", "head"], cwd=repo, env=env, check=True, capture_output=True)


def test_the_gate_fails_when_head_lowers_coverage(tmp_path):
    repo, run_env = fixture(tmp_path)
    result = run_gate(repo, run_env, "70 51", "71 51")
    assert result.returncode != 0, result.stdout + result.stderr
    assert "below base" in result.stdout


def test_the_gate_passes_when_head_matches_base(tmp_path):
    repo, run_env = fixture(tmp_path)
    result = run_gate(repo, run_env, "96 91", "96 91")
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_gate_uses_the_base_filters_on_head(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(repo, run_env, "scripts/coverage-filters.sh", "this is not bash\n")
    result = run_gate(repo, run_env, "96 91", "96 91")
    assert result.returncode == 0, result.stdout + result.stderr
