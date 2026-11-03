"""The plan job runs only the sections whose inputs changed.

ci.yml starts with a plan job that restores each task's last successful
inputs from the latest master run and reports one output per scope from
`task --status`. Each section job needs the plan and runs only when its
scope is selected; pushes to master and workflow_dispatch run every job,
forced, and save the checksums. A cache miss runs everything, as does a
task Task itself cannot check, which fails safe to FULL.
"""

import os
import subprocess
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_taskfile

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"

# One plan output per task the status loop checks, keyed as the job
# conditions name them: the task name, with the e2e legs sharing e2e.
OUTPUTS = (
    "lint",
    "scripts",
    "commits",
    "core",
    "gen",
    "bridge",
    "cpp",
    "e2e",
    "coverage",
    "docs",
    "mermaid",
    "examples",
    "budgets",
    "fuzz",
    "nightly",
    "readme",
    "full",
)

# Each section job and the plan output that selects it. The e2e legs share
# the e2e output and split on its backend value.
SECTION_SCOPE = {
    "lint": "lint",
    "scripts": "scripts",
    "core": "core",
    "gen": "gen",
    "cpp": "cpp",
    "docs": "docs",
    "mermaid": "mermaid",
    "readme": "readme",
    "coverage": "coverage",
    "bridge": "bridge",
    "fuzz-smoke": "fuzz",
    "build-matrix": "examples",
    "budgets": "budgets",
    "nightly": "nightly",
    "e2e-avr": "e2e",
    "e2e-posix": "e2e",
}

ALWAYS_RUN = ("plan", "bridge-images")

TASKS_WITH_CHECKSUMS = (
    "scripts",
    "core",
    "gen",
    "bridge",
    "cpp",
    "coverage",
    "docs",
    "examples",
    "budgets",
    "readme",
    "fuzz",
    "e2e-avr",
    "e2e-posix",
    "nightly",
    "mermaid",
)


def workflow():
    return yaml.safe_load(WORKFLOW.read_text())


def resolve(ref, event, outputs):
    ref = ref.strip()
    if ref == "github.event_name":
        return event
    if ref.startswith("needs.plan.outputs."):
        return outputs.get(ref.removeprefix("needs.plan.outputs."), "")
    assert ref.startswith(("'", '"')), f"cannot resolve {ref}"
    return ref[1:-1]


def runs(condition, event, outputs):
    for clause in str(condition).split("||"):
        clause = clause.strip()
        for op in ("==", "!="):
            if op in clause:
                left, right = clause.split(op, 1)
                left = resolve(left, event, outputs)
                right = resolve(right, event, outputs)
                if (left == right) == (op == "=="):
                    return True
                break
        else:
            raise AssertionError(f"cannot evaluate {clause}")
    return False


def running_jobs(event, outputs):
    jobs = workflow()["jobs"]
    return sorted(name for name, job in jobs.items() if "if" not in job or runs(job["if"], event, outputs))


def plan_steps():
    return workflow()["jobs"]["plan"]["steps"]


def step_text(step):
    return str(step.get("run", "")) + str(step.get("with", ""))


def test_the_plan_job_exposes_one_output_per_scope():
    plan = workflow()["jobs"]["plan"]
    assert set(plan["outputs"]) == set(OUTPUTS)


def test_the_plan_installs_pinned_task():
    assert any("install-task" in step_text(step) for step in plan_steps())


def test_the_plan_restores_each_checksum_from_master():
    restores = [step for step in plan_steps() if "cache/restore" in str(step.get("uses", ""))]
    paths = [step["with"]["path"] for step in restores]
    for task in TASKS_WITH_CHECKSUMS:
        assert f".task/checksum/{task}" in paths, task
        step = next(s for s in restores if s["with"]["path"] == f".task/checksum/{task}")
        assert f"r2s-task-{task}-" in step["with"]["key"], task
        fallbacks = step["with"]["restore-keys"]
        assert f"r2s-task-{task}-refs/heads/master-" in fallbacks, task


def test_the_plan_checks_each_task_status():
    step = next(s for s in plan_steps() if s.get("id") == "statuses")
    script = step["run"]
    assert 'task --status "$task"' in script
    for task in TASKS_WITH_CHECKSUMS:
        assert task in script, task
    assert 'e2e="both"' in script
    assert "full=" in script


def test_the_plan_carries_no_soak_output():
    """The soak runs on schedule only: ci.yml keeps it off on pull requests."""
    plan = workflow()["jobs"]["plan"]
    assert "soak" not in plan["outputs"]
    nightly = workflow()["jobs"]["nightly"]
    assert "soak" not in nightly["if"]
    assert nightly["with"]["soak"] is False


def test_each_section_job_needs_the_plan_and_gates_on_its_scope():
    jobs = workflow()["jobs"]
    for name, scope in SECTION_SCOPE.items():
        job = jobs[name]
        needs = job["needs"] if isinstance(job["needs"], list) else [job["needs"]]
        assert "plan" in needs, name
        condition = job["if"]
        assert "needs.plan.outputs.full" in condition, name
        assert f"needs.plan.outputs.{scope}" in condition, name


def test_each_section_job_runs_off_the_pull_request():
    jobs = workflow()["jobs"]
    for name in SECTION_SCOPE:
        assert "github.event_name" in jobs[name]["if"], name


def test_a_gated_job_never_needs_a_skippable_job():
    jobs = workflow()["jobs"]
    skippable = {name for name in jobs if name not in ALWAYS_RUN}
    for name, job in jobs.items():
        needs = job.get("needs", [])
        needs = needs if isinstance(needs, list) else [needs]
        assert not (set(needs) & skippable), f"{name} needs a job that can skip"


def jobs_with_saves(jobs):
    """Section jobs carrying a master-gated checksum save step."""
    found = {}
    for name, job in jobs.items():
        for step in job.get("steps", []):
            with_ = step.get("with", {})
            if "cache/save" in str(step.get("uses", "")) and ".task/checksum/" in str(with_.get("path", "")):
                assert step.get("if") == "github.ref == 'refs/heads/master'", name
                found[name] = with_["path"]
    return found


def test_section_jobs_save_their_checksums_on_master():
    saves = jobs_with_saves(workflow()["jobs"])
    assert saves["scripts"] == ".task/checksum/scripts"
    assert saves["core"] == ".task/checksum/core"
    assert saves["gen"] == ".task/checksum/gen"
    assert saves["cpp"] == ".task/checksum/cpp"
    assert saves["docs"] == ".task/checksum/docs"
    assert saves["mermaid"] == ".task/checksum/mermaid"
    assert saves["readme"] == ".task/checksum/readme"
    assert saves["coverage"] == ".task/checksum/coverage"
    assert saves["bridge"] == ".task/checksum/bridge"
    assert saves["fuzz-smoke"] == ".task/checksum/fuzz"
    assert saves["build-matrix"] == ".task/checksum/examples"
    assert saves["budgets"] == ".task/checksum/budgets"
    assert saves["e2e-avr"] == ".task/checksum/e2e-avr"
    assert saves["e2e-posix"] == ".task/checksum/e2e-posix"
    assert "lint" not in saves, "lint writes no checksum: it always runs"
    assert "bridge-images" not in saves, "bridge-images is setup, not a section"
    assert "plan" not in saves
    nightly = yaml.safe_load((ROOT / ".github" / "workflows" / "nightly-jobs.yml").read_text())
    night_saves = jobs_with_saves(nightly["jobs"])
    assert night_saves["fuzz-long"] == ".task/checksum/fuzz"
    assert night_saves["e2e-isolation"] == ".task/checksum/nightly"


def run_statuses(repo, tmp_path):
    """The plan step's own shell, from ci.yml, on a fixture with checksums."""
    step = next(s for s in plan_steps() if s.get("id") == "statuses")
    runner_temp = tmp_path / "runner"
    runner_temp.mkdir(exist_ok=True)
    output = tmp_path / "output"
    output.write_text("")
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(RUNNER_TEMP=str(runner_temp), GITHUB_OUTPUT=str(output))
    proc = subprocess.run(["bash", "-e", "-c", step["run"]], cwd=repo, env=env, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    return dict(line.split("=", 1) for line in output.read_text().split())


def selected_jobs(repo, tmp_path, event="pull_request"):
    return running_jobs(event, run_statuses(repo, tmp_path))


def establish(repo):
    """Record a successful run of every task, as master runs maintain them.

    The default task skips the host-only mermaid check, so it is
    established with an explicit run, like the mermaid job does.
    """
    test_taskfile.run_task(repo)
    test_taskfile.run_task(repo, "mermaid")


def test_a_cache_miss_runs_every_job(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    assert selected_jobs(repo, tmp_path) == sorted(workflow()["jobs"])


def test_a_docs_edit_runs_docs_mermaid_and_lint(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "docs" / "notes.md").write_text("probe\n")
    assert selected_jobs(repo, tmp_path) == [
        "bridge-images",
        "docs",
        "lint",
        "mermaid",
        "plan",
    ]


def test_a_gen_test_edit_runs_only_gen_besides_lint(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "gen" / "tests" / "probe").write_text("probe\nprobe again\n")
    assert selected_jobs(repo, tmp_path) == ["bridge-images", "gen", "lint", "plan"]


def test_a_versions_env_edit_runs_every_job(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "ci" / "versions.env").write_text("TASK_VERSION=1\n")
    assert selected_jobs(repo, tmp_path) == sorted(workflow()["jobs"])


def test_an_isolation_edit_runs_the_nightly_job_not_the_e2e_legs(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "tests" / "e2e" / "test_isolation.py").write_text("probe\nprobe again\n")
    assert selected_jobs(repo, tmp_path) == [
        "bridge-images",
        "lint",
        "nightly",
        "plan",
        "scripts",
    ]


def test_a_broken_taskfile_fails_safe_to_full(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    (repo / "Taskfile.yml").write_text("version: '3'\nnot-tasks: [\n")
    assert selected_jobs(repo, tmp_path) == sorted(workflow()["jobs"])


def test_master_push_runs_every_job(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    test_taskfile.run_task(repo)
    assert selected_jobs(repo, tmp_path, event="push") == sorted(workflow()["jobs"])


def test_workflow_dispatch_runs_every_job(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    test_taskfile.run_task(repo)
    assert selected_jobs(repo, tmp_path, event="workflow_dispatch") == sorted(workflow()["jobs"])
