"""Every tracked file maps to a task or says why not.

`tools/taskfile_coverage.py` replaces the retired scope map's fail-safe:
an unknown path used to run the full suite, and now a tracked file no
task lists fails the check with its name. Files no section reads carry
their reason in the script's allowlist instead.
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'tools' / 'taskfile_coverage.py'


def git(repo, *args):
    return subprocess.run(['git', *args], cwd=repo, capture_output=True, text=True, check=True)


def tiny_repo(tmp_path, files):
    repo = tmp_path / 'repo'
    repo.mkdir()
    git(tmp_path, 'init', '-q', '-b', 'master', str(repo))
    git(repo, 'config', 'user.email', 't@example.com')
    git(repo, 'config', 'user.name', 't')
    (repo / 'Taskfile.yml').write_text(
        "version: '3'\ntasks:\n"
        '  gen:\n    sources:\n      - Taskfile.yml\n      - gen/**\n'
        '    cmds:\n      - true\n'
        '  soak:\n    cmds:\n      - true\n'
        '  bigendian:\n    cmds:\n      - true\n'
    )
    for name, text in files.items():
        target = repo / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', 'test(taskfile): fixture')
    # Untracked on purpose: git ls-files must not see it (no task lists
    # a workflow), while the plan check reads it from disk.
    plan = repo / '.github' / 'workflows' / 'ci.yml'
    plan.parent.mkdir(parents=True, exist_ok=True)
    plan.write_text(
        'jobs:\n  plan:\n    run: |\n      for task in gen; do\n        echo $task\n      done\n'
    )
    return repo


def coverage(repo):
    return subprocess.run(
        ['python3', str(SCRIPT), '--root', str(repo)],
        capture_output=True,
        text=True,
    )


def test_a_covered_tree_passes(tmp_path):
    repo = tiny_repo(tmp_path, {'gen/a.py': 'x\n'})
    proc = coverage(repo)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_an_unlisted_file_fails_naming_it(tmp_path):
    repo = tiny_repo(tmp_path, {'gen/a.py': 'x\n', 'bridge/node.cpp': 'y\n'})
    proc = coverage(repo)
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert 'bridge/node.cpp' in proc.stdout + proc.stderr


def test_an_allowlisted_file_passes_with_its_reason(tmp_path):
    repo = tiny_repo(tmp_path, {'gen/a.py': 'x\n', '.github/workflows/nightly.yml': 'n\n'})
    proc = coverage(repo)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_the_real_tree_is_fully_mapped():
    """The map is complete: every tracked file belongs to a task or the allowlist."""
    proc = coverage(ROOT)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_a_task_missing_from_the_plan_loop_fails_naming_it(tmp_path):
    """A task the plan never checks would silently never run on pull requests."""
    repo = tiny_repo(tmp_path, {'gen/a.py': 'x\n'})
    plan = repo / '.github' / 'workflows' / 'ci.yml'
    plan.write_text(plan.read_text().replace('for task in gen;', 'for task in lint;'))
    proc = coverage(repo)
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert 'gen' in proc.stdout + proc.stderr
