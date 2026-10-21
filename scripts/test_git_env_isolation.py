"""Tests that make throwaway repos must never reach the repository running them.

Git exports GIT_DIR, GIT_INDEX_FILE and the like to hooks, so a suite run by
the pre-push hook inherits them; every git call in a test would then act on
the real repository instead of the test's temporary one.
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def leaky_env(tmp_path: Path) -> dict[str, str]:
    decoy = tmp_path / "decoy"
    subprocess.run(["git", "init", "-q", str(decoy)], check=True)
    env = dict(os.environ)
    env.update({"GIT_DIR": str(decoy / ".git"), "GIT_WORK_TREE": str(decoy), "GIT_INDEX_FILE": str(decoy / "idx")})
    return env


def test_pytest_runs_without_the_callers_git_environment(tmp_path):
    probe = tmp_path / "test_probe.py"
    probe.write_text(
        "import os\n\n\ndef test_probe():\n"
        "    assert not [v for v in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE') if v in os.environ]\n"
    )
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "pytest",
            "-q",
            "-p",
            "no:cacheprovider",
            "--rootdir",
            str(ROOT),
            "-c",
            str(ROOT / "pyproject.toml"),
            "--confcutdir",
            str(ROOT),
            str(probe),
        ],
        cwd=ROOT,
        env=leaky_env(tmp_path),
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_shared_helper_clears_every_local_git_variable(tmp_path):
    script = f"source {ROOT / 'scripts/clean-git-env.sh'}; env"
    out = subprocess.run(["bash", "-c", script], env=leaky_env(tmp_path), capture_output=True, text=True).stdout
    assert [v for v in ("GIT_DIR=", "GIT_WORK_TREE=", "GIT_INDEX_FILE=") if v in out] == []


def test_the_suite_and_the_pre_push_hook_use_the_helper():
    for path in ("scripts/ci-test.sh", ".githooks/pre-push"):
        assert "clean-git-env.sh" in (ROOT / path).read_text(), path
