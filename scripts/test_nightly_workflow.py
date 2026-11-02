"""The nightly and release workflows run what is too long for a PR.

The e2e isolation pairs left the SCRIPTS scope for a reusable workflow
beside the long fuzzing. nightly.yml calls it on schedule and files an
issue when the scheduled run fails; release.yml calls it on a tag before
anything is published. These tests pin that contract from the files.
"""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
JOBS = ROOT / ".github" / "workflows" / "nightly-jobs.yml"
NIGHTLY = ROOT / ".github" / "workflows" / "nightly.yml"
RELEASE = ROOT / ".github" / "workflows" / "release.yml"

FAILURE_TITLE = "Nightly run failed"


def workflow(path):
    assert path.is_file(), f"{path.name} is missing"
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def triggers(doc):
    return doc.get(True, doc.get("on"))


def steps_of(doc, name):
    return doc["jobs"][name]["steps"]


def test_the_nightly_jobs_are_reusable_and_trigger_on_nothing():
    assert triggers(workflow(JOBS)) == {"workflow_call": None}


def test_the_reusable_workflow_holds_fuzzing_and_the_isolation_pairs():
    assert set(workflow(JOBS)["jobs"]) == {"fuzz-long", "e2e-isolation"}


def test_fuzz_long_keeps_its_half_hour_per_target():
    steps = steps_of(workflow(JOBS), "fuzz-long")
    assert any("make test FUZZ=1" in str(step.get("run", "")) for step in steps)
    envs = [step.get("env", {}) or {} for step in steps]
    assert any(str(env.get("R2S_FUZZ_SECONDS")) == "1800" for env in envs)


def test_the_isolation_job_runs_the_nightly_scope_with_both_envs():
    steps = steps_of(workflow(JOBS), "e2e-isolation")
    assert any("./.github/actions/dev-image" in str(step.get("uses", "")) for step in steps)
    runs = [step for step in steps if "make test NIGHTLY=1" in str(step.get("run", ""))]
    assert len(runs) == 1
    env = runs[0].get("env", {})
    assert str(env.get("R2S_E2E_SKIP")) == "test_scenario_8_posix"
    assert str(env.get("R2S_SYNC_BOUND_NS")) == "5000000"


def test_make_forwards_the_nightly_scope_and_its_env():
    line = next(line for line in (ROOT / "Makefile").read_text().splitlines() if line.startswith("PASS_VARS"))
    for var in ("NIGHTLY", "R2S_E2E_SKIP", "R2S_SYNC_BOUND_NS"):
        assert var in line.split(), f"make drops {var} before the container"


def test_nightly_calls_the_reusable_workflow_on_schedule():
    doc = workflow(NIGHTLY)
    on = triggers(doc)
    assert on["schedule"] == [{"cron": "0 3 * * *"}]
    assert "workflow_dispatch" in on
    assert any("nightly-jobs.yml" in str(job.get("uses", "")) for job in doc["jobs"].values())


def test_a_failed_scheduled_nightly_files_or_comments_one_issue():
    doc = workflow(NIGHTLY)
    reporters = [
        job
        for job in doc["jobs"].values()
        if "gh issue create" in "\n".join(str(step.get("run", "")) for step in job.get("steps", []))
    ]
    assert len(reporters) == 1
    reporter = reporters[0]
    assert reporter["permissions"].get("issues") == "write"
    condition = str(reporter.get("if", ""))
    assert "failure()" in condition and "schedule" in condition
    needs = reporter.get("needs", [])
    if isinstance(needs, str):
        needs = [needs]
    assert any("nightly-jobs.yml" in str(doc["jobs"][need].get("uses", "")) for need in needs)
    runs = "\n".join(str(step.get("run", "")) for step in reporter["steps"])
    assert FAILURE_TITLE in runs
    assert "needs-triage" in runs
    assert "gh issue comment" in runs
    assert "GITHUB_RUN_ID" in runs
