"""Per-section change detection: a section reruns only when its inputs change.

The Taskfile holds one task per ci-test.sh section, each with `sources:`
listing what that section reads. `task` hashes those inputs with
`method: checksum` and skips a task whose inputs match its last successful
run, locally and in CI. Lint and commits are the standing cheap gates, so
their tasks list no sources and always run, as CHEAP did before them.

These tests run the real Taskfile in a throwaway repository whose section
runner is a stub: every task command is `bash scripts/ci-test.sh <scope>`,
and the stub records each invocation. Editing a probe file then proves
which tasks go stale, without running any real section.
"""

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parent.parent
TASKFILE = ROOT / "Taskfile.yml"

TASK = shutil.which("task")
needs_task = pytest.mark.skipif(TASK is None, reason="task is not installed")


def taskfile():
    return yaml.safe_load(TASKFILE.read_text())


def tasks():
    return taskfile()["tasks"]


def includes(task):
    """The include patterns of a task, without the exclude maps."""
    return [s for s in task.get("sources", []) if isinstance(s, str)]


def glob_match(pattern, path):
    """Doublestar matching as Task resolves it: `*` stays inside a level."""
    out = []
    i = 0
    while i < len(pattern):
        char = pattern[i]
        if char == "*":
            if pattern[i : i + 3] == "**/":
                out.append("(.*/)?")
                i += 3
            elif pattern[i : i + 2] == "**":
                out.append(".*")
                i += 2
            else:
                out.append("[^/]*")
                i += 1
        elif char == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(char))
            i += 1
    return re.fullmatch("".join(out), path) is not None


def matches(task, path):
    """Whether path matches the task's sources, honoring exclude maps."""
    hit = any(glob_match(pattern, path) for pattern in includes(task))
    if not hit:
        return False
    for source in task.get("sources", []):
        if isinstance(source, dict) and "exclude" in source:
            if glob_match(source["exclude"], path):
                return False
    return True


def probe_for(pattern):
    """A path matching a source pattern, with globs filled in."""
    path = pattern.replace("**/", "probe/").replace("**", "probe").replace("*", "probe")
    return path


STUB_RUNNER = """#!/usr/bin/env bash
echo "stub $*" >> "$R2S_TASK_LOG"
"""

# Files the fixture writes by hand: the Taskfile itself, the stub section
# runner it invokes, and the image definition every task lists.
SPECIAL = {"Taskfile.yml", "scripts/ci-test.sh", "Dockerfile", "ci/versions.env"}


def fixture(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    (repo / "Taskfile.yml").write_text(TASKFILE.read_text())
    for name, task in sorted(tasks().items()):
        if name == "default":
            continue
        for pattern in includes(task):
            probed = probe_for(pattern)
            if probed in SPECIAL:
                continue
            target = repo / probed
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                target.write_text("probe\n")
    (repo / "scripts").mkdir(exist_ok=True)
    (repo / "scripts" / "ci-test.sh").write_text(STUB_RUNNER)
    (repo / "scripts" / "lint-docs.sh").write_text('#!/usr/bin/env bash\necho "stub mermaid" >> "$R2S_TASK_LOG"\n')
    (repo / "Dockerfile").write_text("FROM scratch\n")
    versions = repo / "ci" / "versions.env"
    versions.parent.mkdir(exist_ok=True)
    versions.write_text("TASK_VERSION=0\n")
    return repo


def run_task(repo, *args, env_extra=None):
    log = repo / "ran.log"
    env = {
        **os.environ,
        "R2S_TASK_LOG": str(log),
        **(env_extra or {}),
    }
    proc = subprocess.run(
        ["task", "--concurrency", "1", *args],
        cwd=repo,
        capture_output=True,
        text=True,
        env=env,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    ran = []
    if log.exists():
        ran = [line.split()[1] for line in log.read_text().splitlines() if line.startswith("stub ")]
        log.unlink()
    return ran


def default_tasks():
    return list(tasks()["default"]["deps"])


@needs_task
def test_a_gen_test_edit_reruns_only_gen_besides_the_standing_gates(tmp_path):
    """The acceptance case: editing a gen test reruns only the gen task.

    Lint and commits always run: they are the standing cheap gates, as
    CHEAP was before them. Every other task stays quiet.
    """
    repo = fixture(tmp_path)
    ran = run_task(repo)
    assert "GEN=1" in ran, ran
    gen_tests = [p for p in includes(tasks()["gen"]) if p.startswith("gen/tests/")]
    assert gen_tests, "the gen task lists no gen test inputs"
    edited = probe_for(gen_tests[0])
    for name, task in tasks().items():
        if name in ("default", "gen", "lint", "commits"):
            continue
        assert not matches(task, edited), f"{edited} also matches {name}"
    (repo / edited).write_text("probe\nprobe again\n")
    reran = run_task(repo)
    assert "GEN=1" in reran, reran
    assert "LINT=1" in reran, reran
    assert "COMMITS=1" in reran, reran
    assert sorted(reran) == ["COMMITS=1", "GEN=1", "LINT=1"], reran


@needs_task
def test_a_versions_env_edit_reruns_every_task(tmp_path):
    """The image definition feeds every task, so pinning a version reruns all."""
    repo = fixture(tmp_path)
    run_task(repo)
    (repo / "ci" / "versions.env").write_text("TASK_VERSION=1\n")
    reran = run_task(repo)
    for name in default_tasks():
        task = tasks()[name]
        scope = {"e2e-avr": "E2E=avr", "e2e-posix": "E2E=posix"}.get(name, f"{name.upper()}=1")
        if task.get("sources") is None:
            assert scope in reran, f"standing {scope} did not rerun: {reran}"
            continue
        assert matches(task, "ci/versions.env"), f"ci/versions.env matches no {name} source"
        assert scope in reran, f"{scope} did not rerun: {reran}"


@needs_task
def test_a_second_run_skips_everything_but_the_standing_gates(tmp_path):
    """The checksums replace the suite stamp: a fresh tree is a fast no-op."""
    repo = fixture(tmp_path)
    first = run_task(repo)
    assert len(first) == len(default_tasks()), first
    second = run_task(repo)
    assert sorted(second) == ["COMMITS=1", "LINT=1"], second


@needs_task
def test_a_core_edit_reruns_its_consumers_but_not_scripts_or_nightly(tmp_path):
    """Sections that read the core rerun; the rest stay quiet with core."""
    repo = fixture(tmp_path)
    run_task(repo)
    core_sources = [p for p in includes(tasks()["core"]) if p.startswith("core/")]
    assert core_sources, "the core task lists no core inputs"
    (repo / probe_for(core_sources[0])).write_text("probe\nprobe again\n")
    reran = run_task(repo)
    assert "CORE=1" in reran, reran
    assert "GEN=1" in reran, reran
    assert "BRIDGE=1" in reran, reran
    assert "SCRIPTS=1" not in reran, reran
    assert "NIGHTLY=1" not in reran, reran


def test_lint_and_commits_list_no_sources():
    """The standing gates always run: unlistable inputs must never skip them."""
    assert tasks()["lint"].get("sources") is None, "lint must always run"
    assert tasks()["commits"].get("sources") is None, "commits must always run"


def test_every_conditional_task_lists_the_image_and_machinery():
    """The image definition and the suite machinery feed every section."""
    for name, task in tasks().items():
        if name in ("default", "lint", "commits", "mermaid"):
            continue
        for path in ("Taskfile.yml", ".gitignore", "Dockerfile", "ci/versions.env", "scripts/ci-test.sh"):
            assert matches(task, path), f"{path} matches no {name} source"


def test_mermaid_runs_on_the_host_outside_the_default_task():
    """The mermaid check needs host docker, so no image task may pull it in."""
    assert "mermaid" not in default_tasks()
    assert matches(tasks()["mermaid"], "Taskfile.yml")
    assert matches(tasks()["mermaid"], "ci/versions.env")
    assert matches(tasks()["mermaid"], "README.md")
    assert matches(tasks()["mermaid"], "docs/01-testing.md")
    assert matches(tasks()["mermaid"], "tools/mermaid-check/check.mjs")
    assert not matches(tasks()["mermaid"], "scripts/ci-test.sh")


def test_the_isolation_pairs_belong_to_nightly_not_the_e2e_legs():
    """Only the nightly task reads the isolation file, as the scope map did."""
    assert matches(tasks()["nightly"], "tests/e2e/test_isolation.py")
    assert not matches(tasks()["e2e-avr"], "tests/e2e/test_isolation.py")
    assert not matches(tasks()["e2e-posix"], "tests/e2e/test_isolation.py")


def test_the_soak_checks_belong_to_soak_not_the_e2e_legs():
    """The soak driver and its checks run on schedule, never in the legs."""
    for path in (
        "tests/e2e/run-soak.sh",
        "tests/e2e/e2e_support/soak.py",
        "tests/e2e/test_soak_avr.py",
        "tests/e2e/test_soak_posix.py",
        "tests/e2e/soak_check_test.py",
    ):
        assert matches(tasks()["soak"], path), path
        assert not matches(tasks()["e2e-avr"], path), path
        assert not matches(tasks()["e2e-posix"], path), path
