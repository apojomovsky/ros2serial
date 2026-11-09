"""Per-section change detection: a section reruns only when its inputs change.

The Taskfile holds one task per ci-test.sh section, each with `sources:`
listing what that section reads. `task` hashes those inputs with
`method: checksum` and skips a task whose inputs match its last successful
run, locally and in CI. Lint and commits are the standing cheap gates, so
their tasks list no sources and always run, as CHEAP did before them.

These tests run the real Taskfile in a throwaway repository whose section
runner is a stub: every task command exports its scope
(`<SCOPE>=1 bash scripts/ci-test.sh`), and the stub records the scope it
sees in its environment. Editing a probe file then proves which tasks go
stale, without running any real section.
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


STUB_RUNNER = """#!/bin/sh
# Mirrors ci-test.sh: the scope arrives by environment, never as an
# argument, so a task that passes it as $1 records nothing here. Plain
# sh: Task runs each command with sh, not bash.
log="$R2S_TASK_LOG"
[ -n "${LINT:-}" ] && echo "stub LINT=$LINT" >> "$log"
[ -n "${SCRIPTS:-}" ] && echo "stub SCRIPTS=$SCRIPTS" >> "$log"
[ -n "${COMMITS:-}" ] && echo "stub COMMITS=$COMMITS" >> "$log"
[ -n "${CORE:-}" ] && echo "stub CORE=$CORE" >> "$log"
[ -n "${GEN:-}" ] && echo "stub GEN=$GEN" >> "$log"
[ -n "${BRIDGE:-}" ] && echo "stub BRIDGE=$BRIDGE" >> "$log"
[ -n "${AMENT_LINT:-}" ] && echo "stub AMENT_LINT=$AMENT_LINT" >> "$log"
[ -n "${CPP:-}" ] && echo "stub CPP=$CPP" >> "$log"
[ -n "${E2E:-}" ] && echo "stub E2E=$E2E" >> "$log"
[ -n "${COVERAGE:-}" ] && echo "stub COVERAGE=$COVERAGE" >> "$log"
[ -n "${DOCS:-}" ] && echo "stub DOCS=$DOCS" >> "$log"
[ -n "${EXAMPLES:-}" ] && echo "stub EXAMPLES=$EXAMPLES" >> "$log"
[ -n "${ESP32:-}" ] && echo "stub ESP32=$ESP32" >> "$log"
[ -n "${PIC:-}" ] && echo "stub PIC=$PIC" >> "$log"
[ -n "${BUDGETS:-}" ] && echo "stub BUDGETS=$BUDGETS" >> "$log"
[ -n "${FUZZ:-}" ] && echo "stub FUZZ=$FUZZ" >> "$log"
[ -n "${README:-}" ] && echo "stub README=$README" >> "$log"
[ -n "${NIGHTLY:-}" ] && echo "stub NIGHTLY=$NIGHTLY" >> "$log"
[ -n "${SOAK:-}" ] && echo "stub SOAK=$SOAK" >> "$log"
exit 0
"""

# Files the fixture writes by hand: the Taskfile itself, the stub section
# runner it invokes, and the image definition every task lists.
SPECIAL = {"Taskfile.yml", "scripts/ci-test.sh", "Dockerfile", "ci/versions.env"}


def fixture(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    (repo / "Taskfile.yml").write_text(TASKFILE.read_text())
    for name, task in sorted(tasks().items()):
        if name in ("default", "local"):
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


# Scopes the ambient environment may set: a sharded CI job or an
# explicit `make test SCRIPTS=1` exports its scope, and the fixture tasks
# would inherit it on top of their own and record it too. Callers pass a
# deliberate scope through env_extra instead.
LEAKY_VARS = (
    "LINT",
    "SCRIPTS",
    "COMMITS",
    "CORE",
    "GEN",
    "BRIDGE",
    "AMENT_LINT",
    "CPP",
    "E2E",
    "COVERAGE",
    "DOCS",
    "EXAMPLES",
    "ESP32",
    "PIC",
    "BUDGETS",
    "FUZZ",
    "README",
    "NIGHTLY",
    "SOAK",
    "R2S_FULL",
    "R2S_SUITE",
)


def run_task(repo, *args, env_extra=None):
    log = repo / "ran.log"
    env = {key: value for key, value in os.environ.items() if key not in LEAKY_VARS}
    env["R2S_TASK_LOG"] = str(log)
    env.update(env_extra or {})
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
def test_a_gen_test_edit_reruns_gen_and_coverage_besides_the_standing_gates(tmp_path):
    """The acceptance case: editing a gen test reruns gen and the gate.

    Lint and commits always run: they are the standing cheap gates, as
    CHEAP was before them. The coverage gate measures the gen tests, so it
    reruns with them. Every other task stays quiet.
    """
    repo = fixture(tmp_path)
    ran = run_task(repo)
    assert "GEN=1" in ran, ran
    gen_tests = [p for p in includes(tasks()["gen"]) if p.startswith("gen/tests/")]
    assert gen_tests, "the gen task lists no gen test inputs"
    edited = probe_for(gen_tests[0])
    for name, task in tasks().items():
        if name in ("default", "local", "gen", "coverage", "ament_lint", "lint", "commits"):
            continue
        assert not matches(task, edited), f"{edited} also matches {name}"
    (repo / edited).write_text("probe\nprobe again\n")
    reran = run_task(repo)
    assert "GEN=1" in reran, reran
    assert "COVERAGE=1" in reran, reran
    assert "AMENT_LINT=1" in reran, reran
    assert "LINT=1" in reran, reran
    assert "COMMITS=1" in reran, reran
    assert sorted(reran) == ["AMENT_LINT=1", "COMMITS=1", "COVERAGE=1", "GEN=1", "LINT=1"], reran


def test_a_mutation_script_edit_reruns_scripts_but_no_suite_task():
    """Mutation testing never runs in a pull request: its scripts only wake the scripts task."""
    for path in ("scripts/mutation_triage.py", "scripts/run-mutation.sh"):
        assert matches(tasks()["scripts"], path), path
        for name, task in tasks().items():
            if name in ("default", "scripts"):
                continue
            assert not matches(task, path), f"{path} also matches {name}"


@needs_task
def test_an_esp32_script_edit_reruns_esp32_budgets_and_scripts(tmp_path):
    """Editing the ESP32 build stales its task, the budgets gate that calls
    it, and the scripts task that owns every script. Lint and commits always
    run; every other task stays quiet.
    """
    repo = fixture(tmp_path)
    ran = run_task(repo)
    assert "ESP32=1" in ran, ran
    edited = "scripts/build-examples-esp32.sh"
    for name, task in tasks().items():
        if name in ("default", "esp32", "budgets", "scripts", "lint", "commits"):
            continue
        assert not matches(task, edited), f"{edited} also matches {name}"
    (repo / edited).write_text("probe\nprobe again\n")
    reran = run_task(repo)
    assert sorted(reran) == ["BUDGETS=1", "COMMITS=1", "ESP32=1", "LINT=1", "SCRIPTS=1"], reran


@needs_task
def test_a_pic_script_edit_reruns_pic_and_scripts_besides_lint(tmp_path):
    """Editing the canary build stales its task and the scripts task that
    owns every script. Lint and commits always run; every other task,
    including the budgets gate, stays quiet.
    """
    repo = fixture(tmp_path)
    ran = run_task(repo)
    assert "PIC=1" in ran, ran
    edited = "scripts/build-pic-canary.sh"
    for name, task in tasks().items():
        if name in ("default", "pic", "scripts", "lint", "commits"):
            continue
        assert not matches(task, edited), f"{edited} also matches {name}"
    (repo / edited).write_text("probe\nprobe again\n")
    reran = run_task(repo)
    assert sorted(reran) == ["COMMITS=1", "LINT=1", "PIC=1", "SCRIPTS=1"], reran


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
    """Task checksums make a fresh tree a fast no-op."""
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
        if name in ("default", "local", "lint", "commits", "mermaid", "bigendian"):
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


# Source roots with a single owning section task: a new package lands as
# a new top-level directory, so every tracked file under each one must be
# an input of that task, or the section stays fresh over the change.
OWNED_ROOTS = {"gen": "gen", "core": "core", "bridge": "bridge", "cpp": "cpp"}


def test_every_top_level_package_directory_is_an_input_of_its_section_task():
    """A new package must wake its section: gen/r2s_rosidl once did not."""
    for root, owner in OWNED_ROOTS.items():
        task = tasks()[owner]
        children = sorted(path for path in (ROOT / root).iterdir() if path.is_dir())
        assert children, f"no package directory under {root}"
        for child in children:
            proc = subprocess.run(
                ["git", "ls-files", child.name],
                cwd=ROOT / root,
                capture_output=True,
                text=True,
                check=True,
            )
            tracked = proc.stdout.splitlines()
            if not tracked:
                # Build output from a test earlier in this session lands
                # untracked and ignored: no tracked file means no package.
                continue
            for path in tracked:
                assert matches(task, f"{root}/{path}"), f"{root}/{path} matches no {owner} source"


def test_bigendian_runs_on_the_host_outside_the_default_task():
    """The big-endian check needs host docker, so no image task may pull it in."""
    assert "bigendian" not in default_tasks()
    assert matches(tasks()["bigendian"], "Taskfile.yml")
    assert matches(tasks()["bigendian"], "ci/versions.env")
    assert matches(tasks()["bigendian"], "docker/bigendian/Dockerfile")
    assert matches(tasks()["bigendian"], "scripts/run-bigendian.sh")
    assert matches(tasks()["bigendian"], "scripts/bigendian-suite.sh")
    assert matches(tasks()["bigendian"], "core/src/codec.c")
    assert matches(tasks()["bigendian"], "spec/vectors/frames/hello_req.yaml")
    assert matches(tasks()["bigendian"], "gen/r2s_gen/reference/messages.py")
    assert not matches(tasks()["bigendian"], "scripts/ci-test.sh")


def test_the_big_endian_scripts_belong_to_bigendian_not_the_section_tasks():
    """The big-endian driver only wakes its task and the scripts task."""
    for path in ("scripts/run-bigendian.sh", "scripts/bigendian-suite.sh"):
        assert matches(tasks()["scripts"], path), path
        assert matches(tasks()["bigendian"], path), path
        for name, task in tasks().items():
            if name in ("default", "local", "scripts", "bigendian", "lint", "commits"):
                continue
            assert not matches(task, path), f"{path} also matches {name}"


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


@needs_task
def test_soak_runs_on_schedule_outside_the_default_task(tmp_path):
    """The soak is schedule-only, so the default task leaves it out.

    A cold tree's unscoped run would otherwise start the multi-hour
    soak. Explicit runs still select it, and a versions edit stales it.
    """
    assert "soak" not in default_tasks()
    assert matches(tasks()["soak"], "ci/versions.env")
    repo = fixture(tmp_path)
    run_task(repo)
    (repo / "ci" / "versions.env").write_text("TASK_VERSION=1\n")
    assert run_task(repo, "soak") == ["SOAK=1"]


def test_the_local_task_is_the_default_set_without_the_ci_only_sections():
    """The pre-push hook and the takeoff run it; CI runs e2e and esp32."""
    ci_only = {"e2e-avr", "e2e-posix", "nightly", "esp32", "pic"}
    local = list(tasks()["local"]["deps"])
    assert local == [name for name in default_tasks() if name not in ci_only], local
    assert not ci_only & set(local), local


def test_the_hook_and_the_takeoff_run_the_local_task():
    hook = (ROOT / ".githooks" / "pre-push").read_text()
    assert "R2S_LOCAL=1 make -s -C" in hook
    takeoff = (ROOT / "scripts" / "pre-pr-check.sh").read_text()
    assert 'R2S_LOCAL=1 bash "$root/scripts/run-tasks.sh"' in takeoff
    pass_vars = (ROOT / "scripts" / "pass-vars.list").read_text().split()
    assert "R2S_LOCAL" in pass_vars
