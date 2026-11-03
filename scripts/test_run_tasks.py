"""The scope-to-task selection: make variables name the tasks to run.

`scripts/run-tasks.sh` maps the ci-test.sh scope variables to Taskfile
task names and runs the stale ones through `task`. `R2S_FULL=1` forces
every task, and naming no scope runs the default task. `R2S_SUITE` points
the runner at a recorder instead of `task`, so the selection is testable
without running any section.
"""

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "run-tasks.sh"


def select(tmp_path, **env):
    recorder = tmp_path / "record.sh"
    seen_args = tmp_path / "args.txt"
    seen_env = tmp_path / "env.txt"
    recorder.write_text(f'#!/usr/bin/env bash\necho "$*" > {seen_args}\nenv > {seen_env}\n')
    recorder.chmod(0o755)
    merged = {k: v for k, v in os.environ.items() if k not in ("R2S_FULL",)}
    for scope in (
        "LINT",
        "SCRIPTS",
        "COMMITS",
        "CORE",
        "GEN",
        "BRIDGE",
        "CPP",
        "E2E",
        "COVERAGE",
        "DOCS",
        "EXAMPLES",
        "BUDGETS",
        "FUZZ",
        "README",
        "NIGHTLY",
    ):
        merged.pop(scope, None)
    merged.update(R2S_SUITE=str(recorder))
    merged.update(env)
    proc = subprocess.run(["bash", str(SCRIPT)], capture_output=True, text=True, env=merged)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    return seen_args.read_text().split(), seen_env.read_text()


def test_one_scope_selects_its_task(tmp_path):
    args, _ = select(tmp_path, CORE="1")
    assert args == ["core"], args


def test_scopes_select_in_suite_order(tmp_path):
    args, _ = select(tmp_path, GEN="1", CORE="1", LINT="1")
    assert args == ["lint", "core", "gen"], args


def test_the_posix_leg_selects_only_its_task(tmp_path):
    args, _ = select(tmp_path, E2E="posix")
    assert args == ["e2e-posix"], args


def test_both_backends_select_both_legs(tmp_path):
    args, _ = select(tmp_path, E2E="both")
    assert args == ["e2e-avr", "e2e-posix"], args


def test_no_scope_selects_the_default_task(tmp_path):
    args, seen = select(tmp_path)
    assert args == [], args
    assert "CORE=1" not in seen


def test_the_caller_environment_reaches_the_runner(tmp_path):
    """The scope variables stay exported: the recorder sees what task would."""
    _, seen = select(tmp_path, CORE="1", COMMIT_RANGE="origin/master..HEAD")
    assert "CORE=1" in seen.splitlines()
    assert "COMMIT_RANGE=origin/master..HEAD" in seen.splitlines()


def test_full_forces_every_task(tmp_path):
    args, _ = select(tmp_path, CORE="1", R2S_FULL="1")
    assert args == ["--force"], args
