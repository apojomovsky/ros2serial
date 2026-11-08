"""The plan job runs only the sections whose inputs changed.

ci.yml starts with a plan job that restores each task's last successful
inputs from the latest master run and reports one output per scope from
`task --status`. Each section job needs the plan and runs only when its
scope is selected; pushes to master and workflow_dispatch run every job,
forced. A single-job section saves its checksum itself on master; a
matrix section saves through a final job needing every leg, so one
passing leg never marks the section fresh while a sibling failed. A
cache miss runs everything, as does a task Task itself cannot check,
which fails safe to FULL.
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
    "ament_lint",
    "cpp",
    "e2e",
    "esp32",
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
    "ament-lint": "ament_lint",
    "fuzz-smoke": "fuzz",
    "build-matrix": "examples",
    "esp32": "esp32",
    "budgets": "budgets",
    "nightly": "nightly",
    "e2e-avr": "e2e",
    "e2e-posix": "e2e",
}

ALWAYS_RUN = ("plan", "bridge-images")

# Refs the condition evaluator pretends the run is on.
PR_REF = "refs/pull/1/merge"
MASTER_REF = "refs/heads/master"

# Matrix sections save through a final job, keyed by the leg job: the
# save needs every leg, so the checksum lands only when the whole
# section passed. The legs upload their checksum for it instead.
SECTION_SAVE = {
    "scripts": "scripts-save",
    "bridge": "bridge-save",
    "e2e-posix": "e2e-posix-save",
}

TASKS_WITH_CHECKSUMS = (
    "scripts",
    "core",
    "gen",
    "bridge",
    "ament_lint",
    "cpp",
    "coverage",
    "docs",
    "examples",
    "esp32",
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


def resolve(ref, event, ref_name, outputs):
    ref = ref.strip()
    if ref == "github.event_name":
        return event
    if ref == "github.ref":
        return ref_name
    if ref.startswith("needs.plan.outputs."):
        return outputs.get(ref.removeprefix("needs.plan.outputs."), "")
    assert ref.startswith(("'", '"')), f"cannot resolve {ref}"
    return ref[1:-1]


def eval_condition(condition, event, ref, outputs, labels=()):
    """Evaluate a ci.yml boolean expression, with pull request label support.

    Beyond ==, !=, &&, || and parentheses this handles the label opt-in
    the nightly caller uses: contains with the pull request label names.
    """
    body = str(condition).removeprefix("${{").removesuffix("}}").strip()
    pos = 0

    def skip():
        nonlocal pos
        while pos < len(body) and body[pos].isspace():
            pos += 1

    def parse_or():
        nonlocal pos
        value = parse_and()
        while True:
            skip()
            if body.startswith("||", pos):
                pos += 2
                value = bool(parse_and()) or bool(value)
                continue
            return bool(value)

    def parse_and():
        nonlocal pos
        value = parse_atom()
        while True:
            skip()
            if body.startswith("&&", pos):
                pos += 2
                value = bool(parse_atom()) and bool(value)
                continue
            return bool(value)

    def parse_atom():
        nonlocal pos
        skip()
        if body.startswith("(", pos):
            pos += 1
            value = parse_or()
            skip()
            assert body.startswith(")", pos), f"missing ) in {condition}"
            pos += 1
            return value
        if body.startswith("contains(", pos):
            end = body.index(")", pos)
            target, want = (part.strip() for part in body[pos + len("contains(") : end].split(",", 1))
            pos = end + 1
            assert target == "github.event.pull_request.labels.*.name", target
            assert len(want) >= 2 and want[0] == want[-1] and want[0] in ("'", '"'), want
            return want[1:-1] in labels
        end = len(body)
        for token in ("&&", "||", ")"):
            found = body.find(token, pos)
            if found != -1:
                end = min(end, found)
        text = body[pos:end].strip()
        pos = end
        for op in ("==", "!="):
            if op in text:
                left, right = text.split(op, 1)
                left = resolve(left, event, ref, outputs)
                right = resolve(right, event, ref, outputs)
                return (left == right) if op == "==" else (left != right)
        raise AssertionError(f"cannot evaluate {text}")

    value = parse_or()
    skip()
    assert pos == len(body), f"trailing text in {condition}"
    return value


def runs(condition, event, ref, outputs, labels=()):
    return eval_condition(condition, event, ref, outputs, labels)


def running_jobs(event, outputs, ref=PR_REF, labels=()):
    jobs = workflow()["jobs"]
    return sorted(name for name, job in jobs.items() if "if" not in job or runs(job["if"], event, ref, outputs, labels))


def github_value(operand, event):
    operand = operand.strip()
    for op in ("==", "!="):
        if op in operand:
            left, right = operand.split(op, 1)
            left = resolve(left, event, PR_REF, {})
            right = resolve(right, event, PR_REF, {})
            return (left == right) == (op == "==")
    return resolve(operand, event, PR_REF, {})


def eval_env(expr, event):
    expr = str(expr).removeprefix("${{").removesuffix("}}").strip()
    for branch in expr.split("||"):
        value = True
        for operand in branch.split("&&"):
            got = github_value(operand, event)
            if got in (False, ""):
                value = False
                break
            value = got
        if value not in (False, ""):
            return "" if value is True else value
    return ""


def workflow_env(event):
    return {key: eval_env(value, event) for key, value in workflow().get("env", {}).items()}


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
    save_jobs = set(SECTION_SAVE.values())
    for name, job in jobs.items():
        needs = job.get("needs", [])
        needs = needs if isinstance(needs, list) else [needs]
        if name in save_jobs:
            continue
        assert not (set(needs) & skippable), f"{name} needs a job that can skip"


def test_each_save_job_needs_only_the_plan_and_its_legs():
    """The save skips with its legs, which is the rule: no run, no save."""
    jobs = workflow()["jobs"]
    for leg, save in SECTION_SAVE.items():
        needs = jobs[save].get("needs", [])
        needs = needs if isinstance(needs, list) else [needs]
        assert set(needs) == {"plan", leg}, save


def nightly_jobs():
    return yaml.safe_load((ROOT / ".github" / "workflows" / "nightly-jobs.yml").read_text())["jobs"]


def jobs_with_saves(jobs):
    """Jobs carrying a master-gated checksum save step."""
    found = {}
    for name, job in jobs.items():
        for step in job.get("steps", []):
            with_ = step.get("with", {})
            if "cache/save" in str(step.get("uses", "")) and ".task/checksum/" in str(with_.get("path", "")):
                step_gated = step.get("if") == "github.ref == 'refs/heads/master'"
                job_gated = job.get("if") == "github.ref == 'refs/heads/master'"
                assert step_gated or job_gated, name
                found[name] = with_["path"]
    return found


def test_section_jobs_save_their_checksums_on_master():
    saves = jobs_with_saves(workflow()["jobs"])
    assert saves["core"] == ".task/checksum/core"
    assert saves["gen"] == ".task/checksum/gen"
    assert saves["cpp"] == ".task/checksum/cpp"
    assert saves["docs"] == ".task/checksum/docs"
    assert saves["mermaid"] == ".task/checksum/mermaid"
    assert saves["readme"] == ".task/checksum/readme"
    assert saves["coverage"] == ".task/checksum/coverage"
    assert saves["fuzz-smoke"] == ".task/checksum/fuzz"
    assert saves["build-matrix"] == ".task/checksum/examples"
    assert saves["esp32"] == ".task/checksum/esp32"
    assert saves["budgets"] == ".task/checksum/budgets"
    assert saves["e2e-avr"] == ".task/checksum/e2e-avr"
    assert saves["scripts-save"] == ".task/checksum/scripts"
    assert saves["bridge-save"] == ".task/checksum/bridge"
    assert saves["ament-lint"] == ".task/checksum/ament_lint"
    assert saves["e2e-posix-save"] == ".task/checksum/e2e-posix"
    assert "lint" not in saves, "lint writes no checksum: it always runs"
    assert "bridge-images" not in saves, "bridge-images is setup, not a section"
    assert "plan" not in saves
    night_saves = jobs_with_saves(nightly_jobs())
    assert night_saves["fuzz-long"] == ".task/checksum/fuzz"
    assert night_saves["e2e-isolation"] == ".task/checksum/nightly"


def test_no_matrix_job_saves_a_checksum():
    """A passing leg must not save its section: a red sibling would vanish
    from pull requests until the inputs change again."""
    checked = []
    for name, jobs in (("ci", workflow()["jobs"]), ("nightly", nightly_jobs())):
        for job_name, job in jobs.items():
            if "matrix" not in job.get("strategy", {}):
                continue
            checked.append(f"{name}/{job_name}")
            for step in job.get("steps", []):
                with_ = step.get("with", {})
                saving = "cache/save" in str(step.get("uses", "")) and ".task/checksum/" in str(with_.get("path", ""))
                assert not saving, f"{name}/{job_name} saves a checksum per leg"
    assert checked, "no matrix job found to check"


def test_each_matrix_section_has_a_save_job():
    """The save needs every leg and restores the section checksum on master."""
    jobs = workflow()["jobs"]
    for leg, save in SECTION_SAVE.items():
        job = jobs[save]
        assert job["if"] == "github.ref == 'refs/heads/master'", save
        downloads = [step for step in job.get("steps", []) if "download-artifact" in str(step.get("uses", ""))]
        assert len(downloads) == 1, save
        assert downloads[0]["with"].get("path") == f".task/checksum/{leg}", save
        assert f"r2s-checksum-{leg}-" in str(downloads[0]["with"].get("pattern", "")), save


def test_matrix_legs_upload_their_checksum_for_the_save_job():
    jobs = workflow()["jobs"]
    for leg in SECTION_SAVE:
        uploads = [step for step in jobs[leg].get("steps", []) if "upload-artifact" in str(step.get("uses", ""))]
        assert len(uploads) == 1, leg
        assert f"r2s-checksum-{leg}-" in str(uploads[0]["with"].get("name", "")), leg
        assert uploads[0]["with"].get("path") == f".task/checksum/{leg}", leg
        assert uploads[0].get("if") == "success() && github.ref == 'refs/heads/master'", leg


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


def selected_jobs(repo, tmp_path, event="pull_request", ref=PR_REF, labels=()):
    return running_jobs(event, run_statuses(repo, tmp_path), ref, labels)


def jobs_without_saves():
    return sorted(set(workflow()["jobs"]) - set(SECTION_SAVE.values()))


def establish(repo):
    """Record a successful run of every task, as master runs maintain them.

    The default task skips the host-only mermaid check, so it is
    established with an explicit run, like the mermaid job does.
    """
    test_taskfile.run_task(repo)
    test_taskfile.run_task(repo, "mermaid")


def test_save_jobs_run_on_master_only_after_their_legs(tmp_path):
    """A pull request never saves, even stale: the legs upload for master."""
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    pr_jobs = selected_jobs(repo, tmp_path)
    assert not (set(SECTION_SAVE.values()) & set(pr_jobs))
    with (repo / "Taskfile.yml").open("a") as handle:
        handle.write("\n# probe\n")
    stale_pr_jobs = selected_jobs(repo, tmp_path)
    assert "scripts" in stale_pr_jobs
    assert "bridge" in stale_pr_jobs
    assert "e2e-posix" in stale_pr_jobs
    assert not (set(SECTION_SAVE.values()) & set(stale_pr_jobs))
    master_jobs = selected_jobs(repo, tmp_path, event="push", ref=MASTER_REF)
    assert set(SECTION_SAVE.values()) <= set(master_jobs)


def test_a_cache_miss_runs_every_job_but_the_saves(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    assert selected_jobs(repo, tmp_path) == jobs_without_saves()


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


def test_a_gen_test_edit_runs_gen_and_coverage_besides_lint(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "gen" / "tests" / "probe").write_text("probe\nprobe again\n")
    assert selected_jobs(repo, tmp_path) == ["ament-lint", "bridge-images", "coverage", "gen", "lint", "plan"]


def test_an_esp32_script_edit_runs_esp32_budgets_and_scripts_besides_lint(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "scripts" / "build-examples-esp32.sh").write_text("probe\nprobe again\n")
    assert selected_jobs(repo, tmp_path) == [
        "bridge-images",
        "budgets",
        "esp32",
        "lint",
        "plan",
        "scripts",
    ]


def test_a_versions_env_edit_runs_every_job_but_the_saves(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "ci" / "versions.env").write_text("TASK_VERSION=1\n")
    assert selected_jobs(repo, tmp_path) == jobs_without_saves()


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
    assert selected_jobs(repo, tmp_path) == jobs_without_saves()


def test_master_push_runs_every_job(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    test_taskfile.run_task(repo)
    assert selected_jobs(repo, tmp_path, event="push", ref=MASTER_REF) == sorted(workflow()["jobs"])


def test_workflow_dispatch_runs_every_job(tmp_path):
    repo = test_taskfile.fixture(tmp_path)
    test_taskfile.run_task(repo)
    assert selected_jobs(repo, tmp_path, event="workflow_dispatch", ref=MASTER_REF) == sorted(workflow()["jobs"])


def test_pull_requests_do_not_force_the_suite():
    assert workflow_env("pull_request").get("R2S_FULL", "") == ""


def test_master_runs_force_the_suite():
    for event in ("push", "schedule", "workflow_dispatch"):
        assert workflow_env(event).get("R2S_FULL") == "1", event


def nightly_call():
    """The single ci.yml job calling the reusable nightly jobs."""
    calls = {name: job for name, job in workflow()["jobs"].items() if "nightly-jobs.yml" in str(job.get("uses", ""))}
    assert len(calls) == 1, f"ci.yml should call nightly-jobs.yml once: {sorted(calls)}"
    return next(iter(calls.values()))


def fuzz_long_runs(event, outputs, labels=()):
    """Whether the nightly call enables the long fuzzing for an event."""
    return eval_condition(str((nightly_call().get("with") or {}).get("fuzz", "")), event, PR_REF, outputs, labels)


def test_a_fuzz_edit_runs_fuzz_smoke_but_not_fuzz_long_without_the_label(tmp_path):
    """The long fuzzing stays off pull requests: fuzz sources select the smoke, never the two hours."""
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "core" / "probe").write_text("probe\nprobe again\n")
    (repo / "tests" / "e2e" / "test_isolation.py").write_text("probe\nprobe again\n")
    outputs = run_statuses(repo, tmp_path)
    assert outputs["fuzz"] == "1"
    assert outputs["nightly"] == "1"
    jobs = running_jobs("pull_request", outputs)
    assert "fuzz-smoke" in jobs
    assert "nightly" in jobs
    assert not fuzz_long_runs("pull_request", outputs)


def test_the_fuzz_long_label_opts_a_fuzz_edit_into_the_nightly_caller(tmp_path):
    """The opt-in runs the long fuzzing even when the plan selects no nightly scope."""
    repo = test_taskfile.fixture(tmp_path)
    establish(repo)
    (repo / "core" / "probe").write_text("probe\nprobe again\n")
    outputs = run_statuses(repo, tmp_path)
    assert outputs["fuzz"] == "1"
    assert outputs["nightly"] == ""
    assert "nightly" not in running_jobs("pull_request", outputs)
    labelled = running_jobs("pull_request", outputs, labels=("fuzz-long",))
    assert "nightly" in labelled
    assert fuzz_long_runs("pull_request", outputs, labels=("fuzz-long",))


def test_push_and_dispatch_keep_the_fuzz_selection_for_the_long_fuzzing():
    """Master and manual runs keep fuzz-long: only pull requests need the label."""
    for event in ("push", "workflow_dispatch"):
        assert fuzz_long_runs(event, {"fuzz": "1"})
        assert not fuzz_long_runs(event, {"fuzz": ""})


def test_the_schedule_keeps_the_long_fuzzing_and_the_release_skips_it():
    """nightly.yml leaves the fuzz input at its default; the tag publish still skips the time-boxed run."""
    nightly = yaml.safe_load((ROOT / ".github" / "workflows" / "nightly.yml").read_text())
    calls = [job for job in nightly["jobs"].values() if "nightly-jobs.yml" in str(job.get("uses", ""))]
    assert len(calls) == 1
    assert "fuzz" not in (calls[0].get("with") or {})
    release = yaml.safe_load((ROOT / ".github" / "workflows" / "release.yml").read_text())
    gated = [job for job in release["jobs"].values() if "nightly-jobs.yml" in str(job.get("uses", ""))]
    assert len(gated) == 1
    assert gated[0].get("with", {}).get("fuzz") is False


def test_the_budgets_job_posts_no_pull_request_comment():
    """The table lives in the job summary; a comment would email every reviewer."""
    lines = (ROOT / ".github/workflows/ci.yml").read_text().splitlines()
    start = lines.index("  budgets:")
    job = [lines[start]]
    for line in lines[start + 1 :]:
        if line[:2] == "  " and line[:3] != "   " and line.strip():
            break
        job.append(line)
    text = "\n".join(job)
    assert "gh pr comment" not in text, "the budgets job must not comment on pull requests"
    assert "budgets_comment.py" not in text
