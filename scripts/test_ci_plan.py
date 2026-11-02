"""The plan job runs only the sections a pull request's diff touches.

ci.yml starts with a plan job that runs scripts/scopes_for_diff.py against
the PR's merge base and exposes one output per scope. Each section job needs
the plan and runs only when its scope is selected; pushes to master and
workflow_dispatch run every job. A FULL result runs every job, as today.
"""

import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scopes_for_diff

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"

# One plan output per scope the script can print, keyed as the job
# condition names them: lowercase scope, E2E keeping its backend value.
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
    "examples",
    "budgets",
    "fuzz",
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
    "readme": "readme",
    "coverage": "coverage",
    "bridge": "bridge",
    "fuzz-smoke": "fuzz",
    "build-matrix": "examples",
    "budgets": "budgets",
    "e2e-avr": "e2e",
    "e2e-posix": "e2e",
}

ALWAYS_RUN = ("plan", "bridge-images")


def workflow():
    return yaml.safe_load(WORKFLOW.read_text())


def plan_outputs(paths):
    scopes = scopes_for_diff.scopes_for_paths(list(paths))
    return {key.lower(): value for key, value in scopes.items()}


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


def running_jobs(event, paths):
    jobs = workflow()["jobs"]
    outputs = plan_outputs(paths)
    return sorted(name for name, job in jobs.items() if "if" not in job or runs(job["if"], event, outputs))


def test_the_plan_job_exposes_one_output_per_scope():
    plan = workflow()["jobs"]["plan"]
    assert set(plan["outputs"]) == set(OUTPUTS)
    steps = plan["steps"]
    assert any("scopes_for_diff.py" in str(step.get("run", "")) for step in steps)


def test_the_plan_scopes_against_the_pull_request_base():
    plan = workflow()["jobs"]["plan"]
    text = "\n".join(str(step.get("run", "")) for step in plan["steps"])
    assert "pull_request.base.ref" in text


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


def test_docs_only_diff_runs_lint_and_docs():
    assert running_jobs("pull_request", ["docs/01-testing.md"]) == [
        "bridge-images",
        "docs",
        "lint",
        "plan",
    ]


def test_bridge_diff_runs_bridge_coverage_and_both_e2e_legs():
    assert running_jobs("pull_request", ["bridge/r2s_bridge/src/node.cpp"]) == [
        "bridge",
        "bridge-images",
        "coverage",
        "e2e-avr",
        "e2e-posix",
        "lint",
        "plan",
    ]


def test_avr_only_diff_runs_only_the_avr_leg():
    assert running_jobs("pull_request", ["tests/e2e/run-avr.sh"]) == [
        "bridge-images",
        "e2e-avr",
        "lint",
        "plan",
    ]


def test_posix_only_diff_runs_only_the_posix_leg():
    assert running_jobs("pull_request", ["ports/posix/src/r2s_posix_firmware.c"]) == [
        "bridge",
        "bridge-images",
        "core",
        "e2e-posix",
        "lint",
        "plan",
    ]


def test_full_diff_runs_every_job():
    assert running_jobs("pull_request", ["Makefile"]) == sorted(workflow()["jobs"])


def test_master_push_runs_every_job():
    assert running_jobs("push", ["docs/01-testing.md"]) == sorted(workflow()["jobs"])


def test_workflow_dispatch_runs_every_job():
    assert running_jobs("workflow_dispatch", ["docs/01-testing.md"]) == sorted(workflow()["jobs"])
