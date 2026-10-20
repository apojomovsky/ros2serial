"""ci-test.sh is the single suite script: it must pass on the tree and fail on a lint error.

Runs the script on a copy of the tracked files, scoped to LINT=1 so the
suite never re-enters this test.
"""

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def copy_tree(dest: Path) -> Path:
    """Copy the working tree as git sees it: tracked plus untracked, minus ignored."""
    listed = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    ).stdout
    for name in filter(None, listed.decode().split("\0")):
        src = ROOT / name
        if src.is_file():
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest / name)
    return dest


def run_lint(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", "scripts/ci-test.sh"],
        cwd=root,
        capture_output=True,
        text=True,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin", "LINT": "1", "HOME": str(root)},
    )


def test_lint_passes_on_the_tree(tmp_path):
    result = run_lint(copy_tree(tmp_path))
    assert result.returncode == 0, result.stdout + result.stderr


def test_lint_fails_on_a_python_lint_error(tmp_path):
    root = copy_tree(tmp_path)
    (root / "scripts" / "broken.py").write_text("import os\n")
    result = run_lint(root)
    assert result.returncode != 0
    assert "broken.py" in result.stdout + result.stderr


def test_lint_fails_on_a_layout_problem(tmp_path):
    root = copy_tree(tmp_path)
    (root / "proposal-v4.md").write_text("")
    result = run_lint(root)
    assert result.returncode != 0
    assert "proposal-v4.md" in result.stdout + result.stderr
