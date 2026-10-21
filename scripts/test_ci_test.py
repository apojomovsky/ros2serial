"""ci-test.sh is the single suite script: it must pass on the tree and fail on a lint error.

Runs the script on a copy of the tracked files, scoped to LINT=1 so the
suite never re-enters this test.
"""

import os
import shutil
import subprocess
import sys
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


SCOPES = ("LINT", "SCRIPTS", "COMMITS", "CORE", "GEN", "BRIDGE", "E2E")


def run_lint(root: Path) -> subprocess.CompletedProcess:
    # Keep the caller's tools (CI installs them outside the system PATH), use this
    # interpreter's directory first, and drop inherited scopes so the nested run is
    # LINT only and never re-enters this test.
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env.get("PATH", "")])
    env["LINT"] = "1"
    return subprocess.run(["bash", "scripts/ci-test.sh"], cwd=root, capture_output=True, text=True, env=env)


def test_lint_passes_on_the_tree(tmp_path):
    result = run_lint(copy_tree(tmp_path))
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_suite_does_not_pass_test_down_to_its_own_tests(tmp_path):
    """The script tests call the ritual, and a ritual that sees TEST=1 runs
    the suite again, so TEST must not reach them."""
    text = (ROOT / "scripts/ci-test.sh").read_text()
    assert "env -u TEST python3 -m pytest scripts tools" in text


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
