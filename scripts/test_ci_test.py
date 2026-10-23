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
        if src.is_symlink():
            # A link copied as a file is a different tree, and the layout gate
            # checks the rulebook alias for being a link.
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            (dest / name).symlink_to(os.readlink(src))
        elif src.is_file():
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
    the suite again, so TEST must not reach them.

    "make test TEST=1" forwards TEST into the container, so the suite has to
    drop it before it starts pytest, or the pair forks there as well.
    """
    seen = tmp_path / "seen.txt"
    # A tree with the suite, the script it sources, and one test that records
    # the environment it was started with. The test writes to a file, because
    # pytest swallows what a test prints. Nothing else is in the tree, so the
    # suite collects the probe and nothing else.
    copy = tmp_path / "copy"
    (copy / "scripts").mkdir(parents=True)
    (copy / "tools").mkdir()
    for name in ("ci-test.sh", "clean-git-env.sh"):
        shutil.copy2(ROOT / "scripts" / name, copy / "scripts" / name)
    (copy / "scripts" / "test_probe_env.py").write_text(
        '"""Records the environment the script tests are started with."""\n'
        "\n"
        "import os\n"
        "\n"
        "\n"
        "def test_the_environment():\n"
        f"    with open({str(seen)!r}, 'w') as f:\n"
        "        f.write(os.environ.get('TEST', ''))\n"
    )
    env = {**os.environ, "TEST": "1", "SCRIPTS": "1"}
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env.get("PATH", "")])
    result = subprocess.run(
        ["bash", "scripts/ci-test.sh"],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert seen.exists(), result.stdout + result.stderr
    assert seen.read_text() == "", "TEST reached the script tests"


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
